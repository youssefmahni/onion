#pragma once

#include <string>
#include <string_view>

namespace onion::cli {
inline constexpr std::string_view component_name{"cli"};

// Prints the usage list for `command` ("" = the top-level command list)
// and, when given, the detailed usage of `subcommand` under it. Returns
// true when it printed something (the caller returns 0).
bool print_cli_help(const std::string& command, const std::string& subcommand);
}
