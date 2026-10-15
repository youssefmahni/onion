#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include <vector>

namespace onion::log {
struct Record;
}
namespace onion::store {
class Db;
}

namespace onion::api::review_json {

// D4 review reads (plan §4.1). The queue item key is the path.

nlohmann::json list(const store::Db& read, std::string reason_filter,
                    std::string cursor, int limit);

nlohmann::json detail(const store::Db& read, std::string path);

// The signed records an accept/retry/leaf decision writes (the same
// records the CLI review command writes). `accept` keeps only the
// selected child indices (an empty selection keeps everything, matching
// the CLI); `retry` carries the note; `leaf` settles without children.
std::vector<log::Record> action_records(const store::Db& read,
                                        std::string path,
                                        std::string action,
                                        const nlohmann::json& body,
                                        std::string* error);

}  // namespace onion::api::review_json
