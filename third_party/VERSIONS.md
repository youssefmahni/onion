# Third-party versions

All fetches use the exact archive URL and SHA-256 shown here. Licenses are recorded from each upstream project.

| Dependency | Version | Source | SHA-256 | License |
|---|---:|---|---|---|
| GoogleTest | 1.15.2 | https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz | `7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926` | BSD-3-Clause |
| Monocypher | 4.0.2 | https://monocypher.org/download/monocypher-4.0.2.tar.gz | `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c` | BSD-2-Clause |
| libcurl (Windows only; Schannel) | 8.11.1 | https://github.com/curl/curl/releases/download/curl-8_11_1/curl-8.11.1.tar.xz | `c7ca7db48b0909743eaef34250da02c19bc61d4f1dcedd6603f109409536ab56` | curl |
| cpp-httplib | 0.18.3 | https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz | `a0567bcd6c3fe5cef1b329b96245119047f876b49e06cc129a36a7a8dffe173e` | MIT |
| SQLite amalgamation | 3.47.2 | https://www.sqlite.org/2024/sqlite-amalgamation-3470200.zip | `aa73d8748095808471deaa8e6f34aa700e37f2f787f4425744f53fdd15a89c40` | Public domain |
| zstd | 1.5.6 | https://github.com/facebook/zstd/archive/refs/tags/v1.5.6.tar.gz | `30f35f71c1203369dc979ecde0400ffea93c27391bfd2ac5a9715d2173d92ff7` | BSD-3-Clause OR GPL-2.0 |
| nlohmann/json | 3.11.3 | https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz | `d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d` | MIT |
| OpenCL Headers | vendored legacy copy | `legacy/third_party/OpenCL-Headers` | N/A (copied in-repository source) | Apache-2.0 |

On macOS Onion links the SDK libcurl; on Linux it links the distribution `libcurl4-openssl-dev` package. Both must satisfy the M2 CMake minimum of libcurl 7.66.0, selected because the multi-client loop uses `curl_multi_poll`. The Windows build instead builds the pinned archive above with Schannel. Certificate verification is never disabled.
