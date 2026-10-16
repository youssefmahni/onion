#include "api/tree_json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

#include "store/store.hpp"
#include "viewcache/viewcache.hpp"

namespace onion::api::tree_json {
namespace {

std::string node_status(const store::Db& read, std::string_view path,
                        std::string* error) {
  const auto review = read.review_state(path, error);
  if (review && (*review == "open" || review->empty())) return "review";
  const auto settle = read.settle_by_path(path, error);
  if (settle) return settle->kept ? "settled" : "leaf";
  return "open";
}

std::vector<std::string> split_csv(std::string_view text) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find(',', start);
    parts.push_back(std::string(
        text.substr(start, end == std::string_view::npos
                               ? std::string_view::npos
                               : end - start)));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return parts;
}

std::string csv_escape(std::string_view value) {
  const bool needs_quoting =
      value.find(',') != std::string_view::npos ||
      value.find('"') != std::string_view::npos ||
      value.find('\n') != std::string_view::npos ||
      value.find('\r') != std::string_view::npos;
  if (!needs_quoting) return std::string(value);
  std::string out;
  out.reserve(value.size() + 2);
  out += '"';
  for (const char c : value) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += '"';
  return out;
}

}  // namespace

nlohmann::json children(const store::Db& read, const viewcache::Cache* cache,
                        std::string path, std::string cursor, int limit) {
  std::string error;
  nlohmann::json result;
  const auto page = read.children_page(path, cursor, limit, &error);
  if (!error.empty()) {
    result["error"] = error;
    return result;
  }
  nlohmann::json items = nlohmann::json::array();
  for (const auto& item : page) {
    nlohmann::json entry;
    entry["path"] = item.path;
    entry["code"] = item.code;
    entry["title"] = item.title;
    entry["layer"] = item.layer;
    entry["status"] = node_status(read, item.path, &error);
    if (cache && cache->ready()) {
      const auto counts = cache->counts_for(item.path, &error);
      entry["children"] = counts ? counts->children : 0;
      entry["subtree"] = counts ? counts->subtree : 0;
    } else {
      entry["children"] = read.children_count(item.path, &error);
      entry["subtree"] = read.subtree_count(item.path, &error);
    }
    items.push_back(std::move(entry));
  }
  result["items"] = std::move(items);
  if (!page.empty()) {
    const bool more =
        read.children_count_after(path, page.back().path, &error) > 0;
    result["has_more"] = more;
    result["next_cursor"] = more ? nlohmann::json(page.back().path)
                                 : nlohmann::json(nullptr);
  } else {
    result["next_cursor"] = nullptr;
    result["has_more"] = false;
  }
  return result;
}

nlohmann::json node(const store::Db& read, const viewcache::Cache* cache,
                    std::string path) {
  std::string error;
  nlohmann::json result;
  const auto node_row = read.node_by_path(path, &error);
  if (!error.empty()) {
    result["error"] = error;
    return result;
  }
  if (!node_row) {
    result["error"] = "not found";
    return result;
  }
  result["path"] = node_row->path;
  result["code"] = node_row->code;
  result["title"] = node_row->title;
  result["layer"] = node_row->layer;
  result["origin"] = node_row->origin;
  result["hlc"] = node_row->hlc;
  result["tombstone"] = node_row->tombstone == 1;
  result["status"] = node_status(read, node_row->path, &error);

  const auto settle = read.settle_by_path(node_row->path, &error);
  if (settle) {
    result["settle"] = {{"kept", settle->kept},
                        {"atomic", settle->atomic},
                        {"attempts", settle->attempts},
                        {"score", settle->score},
                        {"campaign", settle->campaign},
                        {"end", settle->end},
                        {"origin", settle->origin}};
    const auto device = read.member_state(settle->origin, &error);
    if (device) result["settle"]["device_state"] = *device;
  } else {
    result["settle"] = nullptr;
  }

  const auto review = read.review_state(node_row->path, &error);
  result["review"] = review ? nlohmann::json(*review) : nlohmann::json(nullptr);

  const auto attempts = read.attempts_for(node_row->path, &error);
  nlohmann::json attempt_rows = nlohmann::json::array();
  std::int64_t tokens_in = 0, tokens_out = 0;
  double best = -1;
  for (const auto& attempt : attempts) {
    tokens_in += attempt.tokens_in;
    tokens_out += attempt.tokens_out;
    const double score = std::atof(attempt.score.c_str());
    if (score > best) best = score;
    attempt_rows.push_back({{"n", attempt.n},
                            {"score", attempt.score},
                            {"passed", attempt.passed},
                            {"tokens_in", attempt.tokens_in},
                            {"tokens_out", attempt.tokens_out},
                            {"cost", attempt.cost},
                            {"campaign", attempt.campaign},
                            {"origin", attempt.origin}});
  }
  result["attempts"] = std::move(attempt_rows);
  result["attempts_summary"] = {
      {"count", attempts.size()},
      {"best_score", best < 0 ? nlohmann::json(nullptr) : nlohmann::json(best)},
      {"tokens_in", tokens_in},
      {"tokens_out", tokens_out}};

  const auto critiques = read.critiques_for(node_row->path, &error);
  nlohmann::json critique_rows = nlohmann::json::array();
  for (const auto& critique : critiques)
    critique_rows.push_back({{"score", critique.score},
                             {"issues", critique.issues},
                             {"remove", critique.remove},
                             {"add", critique.add},
                             {"origin", critique.origin}});
  result["critiques"] = std::move(critique_rows);

  if (cache && cache->ready()) {
    const auto counts = cache->counts_for(node_row->path, &error);
    result["children"] = counts ? counts->children : 0;
    result["subtree"] = counts ? counts->subtree : 0;
  } else {
    result["children"] = read.children_count(node_row->path, &error);
    result["subtree"] = read.subtree_count(node_row->path, &error);
  }
  return result;
}

nlohmann::json search(const store::Db& read, const viewcache::Cache* cache,
                      std::string query, std::string levels,
                      std::string status, std::string cursor) {
  (void)cursor;  // filters narrow the space first; deep pagination is rare
  std::string error;
  nlohmann::json result;
  std::vector<std::string> path_hits;
  // The "jump" case: a bare path-shaped query pins that node first.
  const bool path_shaped =
      !query.empty() &&
      query.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                              "abcdefghijklmnopqrstuvwxyz"
                              "0123456789-") == std::string::npos;
  if (path_shaped) {
    const auto pinned = read.node_by_path(query, &error);
    if (pinned) path_hits.push_back(pinned->path);
  }
  if (!query.empty() && cache && cache->ready()) {
    // FTS5: each token becomes a prefix term ("plan task" -> "plan* task*").
    std::string fts_query;
    std::size_t start = 0;
    while (start < query.size()) {
      while (start < query.size() &&
             std::isspace(static_cast<unsigned char>(query[start])))
        ++start;
      const std::size_t end = query.find_first_of(" \t", start);
      const std::string token =
          query.substr(start, end == std::string::npos
                                  ? std::string::npos
                                  : end - start);
      if (!token.empty()) {
        if (!fts_query.empty()) fts_query += " ";
        fts_query += "\"" + token + "\"*";
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
    for (const auto& hit : cache->search(fts_query, 200, &error))
      if (std::find(path_hits.begin(), path_hits.end(), hit.path) ==
          path_hits.end())
        path_hits.push_back(hit.path);
  } else if (!query.empty()) {
    for (const auto& node_row : read.search_titles(query, 200, &error))
      if (std::find(path_hits.begin(), path_hits.end(), node_row.path) ==
          path_hits.end())
        path_hits.push_back(node_row.path);
  }
  if (!error.empty()) {
    result["error"] = error;
    return result;
  }

  nlohmann::json items = nlohmann::json::array();
  for (const auto& path : path_hits) {
    const auto node_row = read.node_by_path(path, &error);
    if (!node_row || node_row->tombstone) continue;
    if (!levels.empty()) {
      bool allowed = false;
      for (const auto& level_token : split_csv(levels))
        if (std::atoi(level_token.c_str()) == node_row->layer) allowed = true;
      if (!allowed) continue;
    }
    const std::string item_status = node_status(read, node_row->path, &error);
    if (!status.empty() && item_status != status) continue;
    items.push_back({{"path", node_row->path},
                     {"code", node_row->code},
                     {"title", node_row->title},
                     {"layer", node_row->layer},
                     {"status", item_status}});
  }
  result["items"] = std::move(items);
  result["next_cursor"] = nullptr;
  return result;
}

nlohmann::json stats(const store::Db& read) {
  std::string error;
  nlohmann::json result;
  auto levels = read.level_counts(&error);
  auto statuses = read.status_counts(&error);
  // Copies imported by older binaries never ran the fold deltas: the
  // counters are missing while the tables are not. One on-the-fly pass
  // covers that case (the live node always has fresh counters, and the
  // fallback is gated so the fast path never scans).
  if (levels.empty()) {
    auto node_total = read.prepare(
        "SELECT COUNT(*) FROM nodes WHERE tombstone = 0;", &error);
    std::int64_t live_nodes = 0;
    if (node_total) {
      std::string step_error;
      if (node_total->step_row(&step_error))
        live_nodes = node_total->integer(0);
    }
    if (live_nodes > 0) {
      auto statement = read.prepare(
          "SELECT layer, COUNT(*) FROM nodes WHERE tombstone = 0 GROUP BY "
          "layer ORDER BY layer;",
          &error);
      if (statement) {
        std::string step_error;
        while (statement->step_row(&step_error))
          levels.emplace_back(statement->integer_int(0),
                              statement->integer(1));
      }
    }
  }
  if (statuses.empty()) {
    auto settle_total = read.prepare("SELECT COUNT(*) FROM settles;", &error);
    std::int64_t settles = 0;
    if (settle_total) {
      std::string step_error;
      if (settle_total->step_row(&step_error)) settles = settle_total->integer(0);
    }
    if (settles > 0) {
      auto statement = read.prepare(
          "SELECT end, COUNT(*) FROM settles WHERE end IS NOT NULL AND end "
          "!= '' GROUP BY end;",
          &error);
      if (statement) {
        std::string step_error;
        while (statement->step_row(&step_error))
          statuses.emplace_back(statement->text(0), statement->integer(1));
      }
    }
  }
  std::int64_t nodes_total = 0;
  nlohmann::json level_rows = nlohmann::json::array();
  for (const auto& [layer, count] : levels) {
    nodes_total += count;
    level_rows.push_back({{"layer", layer}, {"nodes", count}});
  }
  result["levels"] = std::move(level_rows);
  result["nodes_total"] = nodes_total;

  nlohmann::json ending_rows = nlohmann::json::array();
  std::int64_t leaves = 0;
  for (const auto& [status, count] : statuses) {
    if (status != "children") leaves += count;
    ending_rows.push_back({{"end", status}, {"n", count}});
  }
  result["endings"] = std::move(ending_rows);
  result["leaves_total"] = leaves;

  auto settle_total = read.prepare("SELECT COUNT(*) FROM settles;", &error);
  std::int64_t settled_total = 0;
  if (settle_total) {
    std::string step_error;
    if (settle_total->step_row(&step_error))
      settled_total = settle_total->integer(0);
  }
  auto atomic_total = read.prepare(
      "SELECT COUNT(*) FROM settles WHERE atomic = 1;", &error);
  std::int64_t atomic_count = 0;
  if (atomic_total) {
    std::string step_error;
    if (atomic_total->step_row(&step_error))
      atomic_count = atomic_total->integer(0);
  }
  auto review_open = read.prepare(
      "SELECT COUNT(*) FROM reviews WHERE state IS NULL OR state != "
      "'decided';",
      &error);
  std::int64_t review_count = 0;
  if (review_open) {
    std::string step_error;
    if (review_open->step_row(&step_error))
      review_count = review_open->integer(0);
  }
  result["settled_total"] = settled_total;
  result["atomic_total"] = atomic_count;
  result["review_open"] = review_count;

  auto campaigns = read.prepare(
      "SELECT campaign, COUNT(*) FROM settles WHERE campaign IS NOT NULL "
      "AND campaign != '' GROUP BY campaign ORDER BY 2 DESC LIMIT 20;",
      &error);
  nlohmann::json campaign_rows = nlohmann::json::array();
  if (campaigns) {
    std::string step_error;
    while (campaigns->step_row(&step_error))
      campaign_rows.push_back(
          {{"campaign", campaigns->text(0)},
           {"settles", campaigns->integer(1)}});
  }
  result["campaigns"] = std::move(campaign_rows);
  if (!error.empty()) result["error"] = error;
  return result;
}

std::string export_csv(const store::Db& read, std::string path) {
  std::string error;
  std::string out = "code,path,title,layer,status,end\n";
  auto statement = read.prepare(
      "SELECT code, path, title, layer FROM nodes WHERE path >= ?1 AND "
      "path < ?2 AND tombstone = 0 ORDER BY path LIMIT 1000000;",
      &error);
  if (!statement) return out;
  const std::string ceiling = path + ".";
  std::string bind_error;
  statement->bind(1, path, &bind_error);
  statement->bind(2, ceiling, &bind_error);
  std::string step_error;
  std::int64_t rows = 0;
  while (statement->step_row(&step_error)) {
    const std::string node_path = statement->text(1);
    out += csv_escape(statement->text(0)) + "," + csv_escape(node_path) +
           "," + csv_escape(statement->text(2)) + "," + statement->text(3) +
           "," + node_status(read, node_path, &error) + ",";
    const auto settle = read.settle_by_path(node_path, &error);
    out += settle ? csv_escape(settle->end) : "";
    out += "\n";
    if (++rows >= 1000000) break;
  }
  return out;
}

}  // namespace onion::api::tree_json
