// libFuzzer target: canonical JSON (decision 0003) and the record round
// trip (C3).  The invariant: canonicalize(parse(x)) is a fixed point.
#include <cstdint>
#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "log/log.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  try {
    const std::string text(reinterpret_cast<const char*>(data), size);
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) return 0;
    const std::string first = onion::log::canonical_json(json);
    const auto reparsed = nlohmann::json::parse(first, nullptr, false);
    if (!reparsed.is_discarded()) {
      const std::string second = onion::log::canonical_json(reparsed);
      if (second != first) std::abort();  // the canonicalizer is not stable
    }
    if (json.is_object()) {
      // The record round trip: from_json must consume what to_json emits.
      const auto record = onion::log::Record::from_json(json);
      const auto emitted = nlohmann::json::parse(record.to_json().dump(), nullptr, false);
      if (!emitted.is_discarded()) {
        const auto again = onion::log::Record::from_json(emitted);
        if (again.type != record.type) std::abort();
      }
    }
  } catch (const std::exception&) {
    // Malformed input throwing is fine; only crashes count.
  }
  return 0;
}
