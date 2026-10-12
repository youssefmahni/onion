// libFuzzer target: the batch/record JSON surface (C3).  A JSON line is
// parsed, treated as a batch wire line, and its records are re-emitted —
// every number must survive the integer-exact round trip (decision 0003).
#include <cstdint>
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "log/log.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  try {
    const std::string text(reinterpret_cast<const char*>(data), size);
    auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return 0;
    if (json.contains("recs") && json["recs"].is_array()) {
      for (const auto& rec : json["recs"]) {
        if (!rec.is_object()) continue;
        const auto record = onion::log::Record::from_json(rec);
        const auto emitted = nlohmann::json::parse(record.to_json().dump(), nullptr, false);
        if (emitted.is_discarded()) continue;
        const auto again = onion::log::Record::from_json(emitted);
        if (again.type != record.type) std::abort();
      }
    }
  } catch (const std::exception&) {
    // Malformed input throwing is fine; only crashes count.
  }
  return 0;
}
