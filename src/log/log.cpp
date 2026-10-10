#include "log.hpp"

#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

extern "C" {
#include <monocypher.h>
}

#include "platform/platform.hpp"

namespace onion::log {
namespace {

std::string escape_json_string(std::string_view value) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(value.size() + 2);
  result += '"';
  for (const unsigned char character : value) {
    switch (character) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character < 0x20) {
          result += "\\u00";
          result += hex[character >> 4U];
          result += hex[character & 0x0fU];
        } else {
          result += static_cast<char>(character);
        }
    }
  }
  result += '"';
  return result;
}

void canonicalize(const nlohmann::json& value, std::string& out) {
  switch (value.type()) {
    case nlohmann::json::value_t::null:
      out += "null";
      return;
    case nlohmann::json::value_t::boolean:
      out += value.get<bool>() ? "true" : "false";
      return;
    case nlohmann::json::value_t::number_integer:
      out += std::to_string(value.get<std::int64_t>());
      return;
    case nlohmann::json::value_t::number_unsigned:
      out += std::to_string(value.get<std::uint64_t>());
      return;
    case nlohmann::json::value_t::number_float:
      throw std::domain_error(
          "canonical_json: floating-point values are forbidden in records");
    case nlohmann::json::value_t::string:
      out += escape_json_string(value.get_ref<const std::string&>());
      return;
    case nlohmann::json::value_t::array: {
      out += '[';
      bool first = true;
      for (const auto& item : value) {
        if (!first) out += ',';
        first = false;
        canonicalize(item, out);
      }
      out += ']';
      return;
    }
    case nlohmann::json::value_t::object: {
      out += '{';
      bool first = true;
      for (const auto& item : value.items()) {
        if (!first) out += ',';
        first = false;
        out += escape_json_string(item.key());
        out += ':';
        canonicalize(item.value(), out);
      }
      out += '}';
      return;
    }
    case nlohmann::json::value_t::binary:
    case nlohmann::json::value_t::discarded:
      throw std::domain_error("canonical_json: unsupported JSON value type");
  }
}

}  // namespace

std::string canonical_json(const nlohmann::json& value) {
  std::string out;
  canonicalize(value, out);
  return out;
}

std::string blake2b_hex(std::string_view bytes) {
  std::uint8_t digest[32]{};
  crypto_blake2b(digest, sizeof(digest),
                 reinterpret_cast<const std::uint8_t*>(bytes.data()),
                 bytes.size());
  static constexpr char digits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(sizeof(digest) * 2);
  for (const std::uint8_t byte : digest) {
    hex += digits[byte >> 4U];
    hex += digits[byte & 0x0fU];
  }
  return hex;
}

nlohmann::json Record::to_json() const {
  return nlohmann::json{{"s", seq}, {"h", hlc}, {"t", type}, {"p", payload}};
}

Record Record::from_json(const nlohmann::json& value) {
  Record record;
  record.seq = value.at("s").get<std::int64_t>();
  record.hlc = value.at("h").get<Hlc>();
  record.type = value.at("t").get<std::string>();
  record.payload = value.value("p", nlohmann::json::object());
  return record;
}

std::string records_json(const std::vector<Record>& records) {
  nlohmann::json array = nlohmann::json::array();
  for (const Record& record : records) array.push_back(record.to_json());
  return canonical_json(array);
}

std::string hash_records_hex(const std::vector<Record>& records) {
  return blake2b_hex(records_json(records));
}

nlohmann::json Batch::to_json() const {
  nlohmann::json records = nlohmann::json::array();
  for (const Record& record : this->records) records.push_back(record.to_json());
  return nlohmann::json{{"v", version}, {"o", origin},       {"b", number},
                        {"prev", prev},   {"hash", hash},     {"sig", sig},
                        {"recs", records}};
}

Batch make_batch(std::string origin, std::uint64_t number,
                 std::string prev_hex, std::vector<Record> records,
                 Signer& signer) {
  Batch batch;
  batch.version = 1;
  batch.origin = std::move(origin);
  batch.number = number;
  batch.prev = std::move(prev_hex);
  batch.hash = hash_records_hex(records);
  const auto unhex = [](const std::string& hex) {
    std::string raw;
    raw.reserve(hex.size() / 2);
    const auto digit = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      const int hi = digit(hex[i]);
      const int lo = digit(hex[i + 1]);
      if (hi < 0 || lo < 0) return std::string{};
      raw += static_cast<char>((hi << 4) | lo);
    }
    return raw;
  };
  batch.sig = signer.sign(unhex(batch.prev) + unhex(batch.hash));
  batch.records = std::move(records);
  return batch;
}

Hlc HlcClock::now() {
  using namespace std::chrono;
  const std::uint64_t wall_ms = static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch())
          .count());
  const std::uint64_t last_physical = last_ >> 16;
  const std::uint64_t last_logical = last_ & 0xffff;
  std::uint64_t physical = wall_ms;
  std::uint64_t logical = 0;
  if (wall_ms <= last_physical) {
    physical = last_physical;
    logical = last_logical + 1;
    if (logical > 0xffff) {
      physical = last_physical + 1;
      logical = 0;
    }
  }
  last_ = (physical << 16) | logical;
  return last_;
}

struct Segments::Impl {
  Options options;
  platform::AppendFile file;
  std::uint64_t active_first_seq{1};
  std::uint64_t batches{0};
  std::uint64_t bytes{0};

  bool opened{false};

  explicit Impl(Options options) : options(std::move(options)) {}
};

Segments::Segments(Options options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Segments::~Segments() = default;
Segments::Segments(Segments&& other) noexcept = default;
Segments& Segments::operator=(Segments&& other) noexcept = default;

namespace {
std::uint64_t first_seq_from_name(const std::string& name) {
  std::uint64_t value = 0;
  for (const char c : name) {
    if (c == '.') break;
    if (c < '0' || c > '9') return 0;
    value = value * 10 + static_cast<std::uint64_t>(c - '0');
  }
  return value;
}
}  // namespace

bool Segments::open(std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(impl_->options.dir, ec);
  if (ec) {
    if (error) *error = "cannot create segment directory: " + ec.message();
    return false;
  }

  std::uint64_t newest = 0;
  std::filesystem::path newest_path;
  for (const auto& entry : std::filesystem::directory_iterator(impl_->options.dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const std::string name = entry.path().filename().string();
    const std::uint64_t first = first_seq_from_name(name);
    if (first == 0 || first > newest) continue;
    newest = first;
    newest_path = entry.path();
  }
  if (newest == 0) {
    newest = 1;
    newest_path = impl_->options.dir / "1.log";
  }

  // Torn-tail cut: drop a trailing batch that has no final LF. (Hash and
  // signature repair arrives in M4.)
  {
    std::ifstream in(newest_path, std::ios::binary | std::ios::ate);
    if (in) {
      const std::uint64_t size = static_cast<std::uint64_t>(in.tellg());
      if (size > 0) {
        in.seekg(-1, std::ios::end);
        char last = 0;
        in.read(&last, 1);
        if (last != '\n') {
          // Keep everything up to and including the previous LF; the torn
          // line is dropped entirely.
          in.close();
          std::ifstream whole(newest_path, std::ios::binary | std::ios::ate);
          const std::uint64_t size = static_cast<std::uint64_t>(whole.tellg());
          whole.seekg(0, std::ios::beg);
          std::string content(static_cast<std::size_t>(size), '\0');
          whole.read(content.data(), static_cast<std::streamsize>(size));
          content.resize(static_cast<std::size_t>(whole.gcount()));
          whole.close();
          const std::size_t pos = content.rfind('\n');
          const std::uint64_t keep =
              pos == std::string::npos ? 0 : static_cast<std::uint64_t>(pos) + 1;
          std::filesystem::resize_file(newest_path, keep, ec);
          if (ec) {
            if (error) *error = "cannot cut torn tail: " + ec.message();
            return false;
          }
        }
      }
    }
  }

  if (!impl_->file.open(newest_path)) {
    if (error) *error = "cannot open active segment " + newest_path.string();
    return false;
  }
  impl_->active_first_seq = newest;
  impl_->bytes = std::filesystem::file_size(newest_path, ec);
  if (ec) impl_->bytes = 0;
  {
    std::ifstream in(newest_path, std::ios::binary);
    std::string line;
    while (std::getline(in, line))
      if (!line.empty()) impl_->batches++;
  }
  impl_->opened = true;
  return true;
}

bool Segments::append(const Batch& batch, std::string* error) {
  if (!impl_->opened) {
    if (error) *error = "segments not open";
    return false;
  }
  const std::string line = canonical_json(batch.to_json()) + "\n";

  const std::uint64_t first = batch.records.empty()
                                  ? impl_->active_first_seq
                                  : static_cast<std::uint64_t>(batch.records.front().seq);
  if (impl_->batches > 0 && impl_->bytes + line.size() > impl_->options.seal_bytes) {
    impl_->file = platform::AppendFile{};
    const std::filesystem::path next = impl_->options.dir /
                                       (std::to_string(first) + ".log");
    if (!impl_->file.open(next)) {
      if (error) *error = "cannot open new segment " + next.string();
      return false;
    }
    impl_->active_first_seq = first;
    impl_->bytes = 0;
    impl_->batches = 0;
  }

  if (!impl_->file.append(line)) {
    if (error) *error = "append failed";
    return false;
  }
  if (!impl_->file.sync()) {
    if (error) *error = "durable sync failed";
    return false;
  }
  impl_->bytes += line.size();
  impl_->batches++;
  return true;
}

std::uint64_t Segments::active_first_seq() const { return impl_->active_first_seq; }
std::uint64_t Segments::total_batches() const { return impl_->batches; }
std::filesystem::path Segments::active_path() const {
  return impl_->options.dir /
         (std::to_string(impl_->active_first_seq) + ".log");
}
std::uint64_t Segments::active_bytes() const { return impl_->bytes; }

}  // namespace onion::log
