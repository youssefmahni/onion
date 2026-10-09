// depot_gpu.hpp — optional OpenCL compute layer (Track A foundation).
//
// Device enumeration + a known-answer self test. Built only when an OpenCL
// loader is found (auto-skipped otherwise, like the onion_gl target); headers
// are vendored in third_party/OpenCL-Headers. No OpenCL types leak through
// this interface — depot_core stays pure.
#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace depot {
namespace gpu {

struct DeviceInfo {
    std::string name;      // CL_DEVICE_NAME
    std::string vendor;    // CL_DEVICE_VENDOR
    std::string version;   // CL_DEVICE_VERSION (e.g. "OpenCL 3.0 pocl")
    std::string driver;    // CL_DRIVER_VERSION
    std::string type;      // "GPU" | "CPU" | "ACCELERATOR" | "OTHER"
    unsigned long long global_mem_bytes = 0;
    std::size_t max_work_group = 0;
    bool fp16 = false;     // supports half precision
    bool fp64 = false;     // supports double precision
};

// true when at least one OpenCL platform/device is usable at runtime.
bool available();

// all devices across all platforms (empty when unavailable).
std::vector<DeviceInfo> list_devices();

// compiled-program choice: pass a different kernel source to exercise build
// failures (tests); returns false and fills `err` on any failure.
bool run_vector_add(const std::string& kernel_src, std::string& err);

// known-answer vector add on the first (GPU-preferred) device.
bool selftest(std::string& err);

// ---- lexical similarity (exact parity with depot::sim_keys) ----
//
// sim_keys(k1,k2) = 0 if either empty; 1 if equal; else
// max(lcs_ratio(k1,k2), jaccard_tokens(k1,k2)) over sorted-token
// canonical strings. The kernel implements this exactly; pairs with
// over-long or unsorted inputs are transparently recomputed on the CPU.
bool sim_keys_batch(const std::vector<std::string>& keys,
                    const std::vector<std::string>& cands,
                    std::vector<double>& out, std::string& err);

// name of the device the similarity engine actually uses ("" when unavailable)
std::string sim_device_name();

// persistent candidate set: uploads the candidate strings once and scores
// batches of keys against all of them (keys x cands matrix, row-major).
class SimIndex {
public:
    SimIndex();
    ~SimIndex();
    SimIndex(SimIndex&&) noexcept;
    SimIndex& operator=(SimIndex&&) noexcept;
    SimIndex(const SimIndex&) = delete;
    SimIndex& operator=(const SimIndex&) = delete;

    bool build(const std::vector<std::string>& cands, std::string& err);
    bool scores(const std::vector<std::string>& keys, std::vector<double>& out,
                std::string& err) const;
    std::size_t size() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gpu
}  // namespace depot
