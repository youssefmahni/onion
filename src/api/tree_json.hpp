#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace onion::store {
class Db;
}
namespace onion::viewcache {
class Cache;
}

namespace onion::api::tree_json {

// §4.1 providers: pure functions over a read-only store handle (and the
// optional viewcache for counts/FTS). Each returns the endpoint's JSON
// (or CSV) body; errors are embedded as {"error": ...}.
nlohmann::json children(const store::Db& read, const viewcache::Cache* cache,
                        std::string path, std::string cursor, int limit);
nlohmann::json node(const store::Db& read, const viewcache::Cache* cache,
                    std::string path);
nlohmann::json search(const store::Db& read, const viewcache::Cache* cache,
                      std::string query, std::string levels,
                      std::string status, std::string cursor);
nlohmann::json stats(const store::Db& read);
std::string export_csv(const store::Db& read, std::string path);

}  // namespace onion::api::tree_json
