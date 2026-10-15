#include "api/review_json.hpp"

#include "log/log.hpp"
#include "store/store.hpp"

#include <set>

namespace onion::api::review_json {

nlohmann::json list(const store::Db& read, std::string reason_filter,
                    std::string cursor, int limit) {
  std::string error;
  nlohmann::json result;
  // Keyset over open items (state open/empty), ordered by path.
  const std::string base =
      "SELECT r.path, r.reason, r.state, r.note, r.hlc, n.title, "
      "(SELECT COUNT(*) FROM attempts a WHERE a.path = r.path) "
      "FROM reviews r LEFT JOIN nodes n ON n.path = r.path "
      "WHERE (r.state IS NULL OR r.state != 'decided') ";
  const std::string sql =
      base + (reason_filter.empty()
                  ? "AND r.path > ?1 ORDER BY r.path LIMIT ?2;"
                  : "AND r.reason = ?2 AND r.path > ?1 ORDER BY r.path "
                    "LIMIT ?3;");
  auto statement = read.prepare(sql, &error);
  if (!statement) {
    result["error"] = error.empty() ? "reviews unavailable" : error;
    return result;
  }
  std::string bind_error;
  if (!statement->bind(1, std::string(cursor), &bind_error)) {
    result["error"] = "bind failed";
    return result;
  }
  if (reason_filter.empty()) {
    if (!statement->bind(2, std::max(1, std::min(limit, 200)),
                         &bind_error)) {
      result["error"] = "bind failed";
      return result;
    }
  } else {
    if (!statement->bind(2, reason_filter, &bind_error) ||
        !statement->bind(3, std::max(1, std::min(limit, 200)),
                         &bind_error)) {
      result["error"] = "bind failed";
      return result;
    }
  }
  nlohmann::json items = nlohmann::json::array();
  std::string step_error;
  while (statement->step_row(&step_error)) {
    items.push_back({{"path", statement->text(0)},
                     {"reason", statement->text(1)},
                     {"state", statement->text(2)},
                     {"note", statement->text(3)},
                     {"hlc", statement->integer(4)},
                     {"title", statement->text(5)},
                     {"attempts", statement->integer(6)}});
  }
  if (!step_error.empty() && error.empty()) error = step_error;
  result["items"] = std::move(items);
  if (!items.empty()) {
    // has_more: one row beyond the last item exists.
    auto more = read.prepare(
        "SELECT 1 FROM reviews WHERE (state IS NULL OR state != 'decided') "
        "AND path > ?1 LIMIT 1;",
        &error);
    bool has_more = false;
    if (more) {
      std::string more_error;
      more->bind(1, items.back()["path"].get<std::string>(), &more_error);
      has_more = more->step_row(&more_error);
    }
    result["next_cursor"] =
        has_more ? nlohmann::json(items.back()["path"]) : nlohmann::json(nullptr);
  } else {
    result["next_cursor"] = nullptr;
  }
  if (!error.empty()) result["error"] = error;
  return result;
}

nlohmann::json detail(const store::Db& read, std::string path) {
  std::string error;
  nlohmann::json result;
  auto review = read.prepare(
      "SELECT reason, state, note, atomic, hlc FROM reviews WHERE path = "
      "?1;",
      &error);
  if (!review) {
    result["error"] = error.empty() ? "review unavailable" : error;
    return result;
  }
  std::string bind_error;
  review->bind(1, path, &bind_error);
  std::string step_error;
  if (!review->step_row(&step_error)) {
    result["error"] = "not found";
    return result;
  }
  result["path"] = path;
  result["reason"] = review->text(0);
  result["state"] = review->text(1);
  result["note"] = review->text(2);
  result["atomic"] = review->integer(3);
  result["hlc"] = review->integer(4);

  const auto node = read.node_by_path(path, &error);
  result["title"] = node ? nlohmann::json(node->title) : nlohmann::json(nullptr);

  // Every attempt for this path.
  const auto attempts = read.attempts_for(path, &error);
  nlohmann::json attempt_rows = nlohmann::json::array();
  for (const auto& attempt : attempts)
    attempt_rows.push_back({{"n", attempt.n},
                            {"score", attempt.score},
                            {"passed", attempt.passed},
                            {"tokens_in", attempt.tokens_in},
                            {"tokens_cached", attempt.tokens_cached},
                            {"tokens_out", attempt.tokens_out},
                            {"cost", attempt.cost},
                            {"campaign", attempt.campaign},
                            {"origin", attempt.origin}});
  result["attempts"] = std::move(attempt_rows);

  // Proposed children (the node's current children rows).
  const auto children = read.children_page(path, "", 200, &error);
  nlohmann::json child_rows = nlohmann::json::array();
  for (std::size_t index = 0; index < children.size(); ++index)
    child_rows.push_back({{"index", index},
                          {"path", children[index].path},
                          {"code", children[index].code},
                          {"title", children[index].title},
                          {"layer", children[index].layer}});
  result["children"] = std::move(child_rows);

  // Check results (critiques) and the settle's atomic label.
  const auto critiques = read.critiques_for(path, &error);
  nlohmann::json critique_rows = nlohmann::json::array();
  for (const auto& critique : critiques)
    critique_rows.push_back({{"score", critique.score},
                             {"issues", critique.issues},
                             {"remove", critique.remove},
                             {"add", critique.add}});
  result["critiques"] = std::move(critique_rows);
  const auto settle = read.settle_by_path(path, &error);
  result["settle"] = settle ? nlohmann::json({{"kept", settle->kept},
                                              {"atomic", settle->atomic},
                                              {"score", settle->score}})
                            : nlohmann::json(nullptr);
  if (!error.empty()) result["error"] = error;
  return result;
}

namespace {

log::Record make_record(std::string type, nlohmann::json payload) {
  log::Record record;
  record.type = std::move(type);
  record.payload = std::move(payload);
  return record;
}

}  // namespace

std::vector<log::Record> action_records(const store::Db& read,
                                        std::string path,
                                        std::string action,
                                        const nlohmann::json& body,
                                        std::string* error) {
  std::vector<log::Record> records;
  if (action == "accept") {
    std::vector<std::string> all_children;
    for (const auto& node : read.nodes_with_path_prefix(path, error)) {
      const std::size_t dash = node.path.rfind('-');
      const std::string parent =
          dash == std::string::npos ? "" : node.path.substr(0, dash);
      if (parent == path) all_children.push_back(node.path);
    }
    std::set<std::size_t> kept_indices;
    if (body.is_object() && body.contains("children") &&
        body["children"].is_array())
      for (const auto& index : body["children"])
        kept_indices.insert(index.get<std::size_t>());
    std::vector<std::string> kept;
    for (std::size_t i = 0; i < all_children.size(); ++i)
      if (kept_indices.empty() || kept_indices.count(i))
        kept.push_back(all_children[i]);
    for (std::size_t i = 0; i < all_children.size(); ++i)
      if (kept_indices.size() && !kept_indices.count(i))
        records.push_back(make_record(
            "TOMBSTONE",
            {{"path", all_children[i]}, {"reason", "not accepted"}}));
    records.push_back(make_record(
        "REVIEW", {{"path", path}, {"decision", "accept"}, {"note", ""}}));
    records.push_back(make_record(
        "SETTLE", {{"path", path},
                   {"kept", static_cast<std::int64_t>(kept.size())},
                   {"atomic", 0},
                   {"attempts", 0},
                   {"score", "0"},
                   {"end", "children"}}));
  } else if (action == "retry") {
    const std::string note =
        body.is_object() ? body.value("note", "") : "";
    records.push_back(make_record(
        "REVIEW", {{"path", path}, {"decision", "retry"}, {"note", note}}));
    records.push_back(
        make_record("REEXPAND", {{"path", path}, {"reason", ""}}));
  } else if (action == "leaf") {
    records.push_back(make_record(
        "REVIEW",
        {{"path", path}, {"decision", "accept"}, {"note", "leaf"}}));
    records.push_back(make_record(
        "SETTLE", {{"path", path}, {"kept", 0}, {"atomic", 0},
                   {"attempts", 0}, {"score", "0"},
                   {"end", "cannot split"}}));
  }
  return records;
}

}  // namespace onion::api::review_json
