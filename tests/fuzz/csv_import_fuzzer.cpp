// libFuzzer target: the CSV/TSV row parser used by the import paths (C3).
// depot::split_csv is the parser every CSV consumer shares.
#include <cstdint>
#include <cstddef>
#include <string>

#include "core/depot_core.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string line(reinterpret_cast<const char*>(data), size);
  const auto columns = depot::split_csv(line);
  // Column access must be safe for any count; the import maps only the
  // first three columns.
  for (const auto& column : columns) (void)column.size();
  return 0;
}
