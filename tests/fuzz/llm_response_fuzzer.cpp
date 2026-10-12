// libFuzzer target: the DeepSeek response parser (C3).
// Exercises every classification and the JSON extraction path on arbitrary
// bytes; nlohmann exceptions and malformed input are normal, only crashes
// (segfault/abort) are findings.
#include <curl/curl.h>

#include <cstdint>
#include <cstddef>
#include <string>

#include "llm/llm.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // Split the input: first byte picks a CURLcode bucket, the rest is the
  // body, so both the transport-classification and the JSON paths get
  // coverage.
  const long code = size == 0 ? CURLE_OK : static_cast<long>(data[0] % 100);
  const long status = size > 0 ? 100 + (data[0] % 5) * 100 : 200;
  const std::string body(reinterpret_cast<const char*>(data + (size > 0 ? 1 : 0)),
                         size > 0 ? size - 1 : 0);
  onion::llm::parse_response(code, status, 0, body, 0.5);
  return 0;
}
