#include "service.hpp"

#include <sstream>

namespace onion::profile {

namespace {
std::string escape_xml(std::string value) {
  const auto replace_all = [&](const std::string& from, const std::string& to) {
    std::size_t at = 0;
    while ((at = value.find(from, at)) != std::string::npos) {
      value.replace(at, from.size(), to);
      at += to.size();
    }
  };
  replace_all("&", "&amp;");
  replace_all("<", "&lt;");
  replace_all(">", "&gt;");
  return value;
}
}  // namespace

std::string systemd_unit(const ServiceConfig& config) {
  std::ostringstream out;
  out << "[Unit]\n"
      << "Description=Onion knowledge engine node\n"
      << "After=network-online.target\n"
      << "Wants=network-online.target\n"
      << "\n"
      << "[Service]\n"
      << "Type=simple\n"
      // No Environment= secrets: the key lives in the OS key store.
      << "ExecStart=" << config.binary << " node run --data-dir "
      << config.data_dir << " --base-url " << config.base_url
      << " --model " << config.model << " --api-port "
      << config.api_port << "\n"
      << "StateDirectory=onion\n"
      << "Restart=always\n"
      << "RestartSec=5\n"
      << "\n"
      << "[Install]\n"
      << "WantedBy=multi-user.target\n";
  return out.str();
}

std::string launchd_plist(const ServiceConfig& config) {
  std::ostringstream out;
  out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
         "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      << "<plist version=\"1.0\">\n"
      << "<dict>\n"
      << "  <key>Label</key><string>com.onion.node</string>\n"
      << "  <key>ProgramArguments</key>\n"
      << "  <array>\n"
      << "    <string>" << escape_xml(config.binary) << "</string>\n"
      << "    <string>node</string>\n"
      << "    <string>run</string>\n"
      << "    <string>--data-dir</string>\n"
      << "    <string>" << escape_xml(config.data_dir) << "</string>\n"
      << "    <string>--base-url</string>\n"
      << "    <string>" << escape_xml(config.base_url) << "</string>\n"
      << "    <string>--model</string>\n"
      << "    <string>" << escape_xml(config.model) << "</string>\n"
      << "    <string>--api-port</string>\n"
      << "    <string>" << config.api_port << "</string>\n"
      << "  </array>\n"
      << "  <key>KeepAlive</key><true/>\n"
      << "  <key>RunAtLoad</key><true/>\n"
      << "</dict>\n"
      << "</plist>\n";
  return out.str();
}

std::string windows_service_script(const ServiceConfig& config) {
  std::ostringstream out;
  out << "@echo off\n"
      << "rem Onion node Windows Service (run as Administrator)\n"
      << "sc create OnionNode binPath= \"\\\"" << config.binary
      << "\\\" node run --data-dir \\\"" << config.data_dir
      << "\\\" --base-url " << config.base_url << " --model " << config.model
      << " --api-port " << config.api_port << "\" start= auto\n"
      << "sc failure OnionNode reset= 86400 actions= restart/5000/restart/5000/restart/5000\n"
      << "sc start OnionNode\n";
  return out.str();
}

}  // namespace onion::profile
