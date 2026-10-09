// depot_gpu.cpp — OpenCL device info + known-answer self test.
#define CL_TARGET_OPENCL_VERSION 120
#include "depot_gpu.hpp"

#include <CL/cl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "depot_core.hpp"

namespace depot {
namespace gpu {

namespace {

struct DeviceRef {
    cl_platform_id platform;
    cl_device_id device;
    cl_device_type type;
};

std::vector<DeviceRef> all_devices() {
    std::vector<DeviceRef> out;
    cl_uint np = 0;
    if (clGetPlatformIDs(0, nullptr, &np) != CL_SUCCESS || np == 0) return out;
    std::vector<cl_platform_id> plats(np);
    if (clGetPlatformIDs(np, plats.data(), nullptr) != CL_SUCCESS) return out;
    for (cl_platform_id p : plats) {
        cl_uint nd = 0;
        if (clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, 0, nullptr, &nd) != CL_SUCCESS || nd == 0)
            continue;
        std::vector<cl_device_id> devs(nd);
        if (clGetDeviceIDs(p, CL_DEVICE_TYPE_ALL, nd, devs.data(), nullptr) != CL_SUCCESS)
            continue;
        for (cl_device_id d : devs) {
            cl_device_type t = 0;
            clGetDeviceInfo(d, CL_DEVICE_TYPE, sizeof t, &t, nullptr);
            out.push_back({p, d, t});
        }
    }
    return out;
}

std::string dev_string(cl_device_id dev, cl_device_info prop) {
    size_t n = 0;
    if (clGetDeviceInfo(dev, prop, 0, nullptr, &n) != CL_SUCCESS || n == 0) return "";
    std::string s(n, '\0');
    if (clGetDeviceInfo(dev, prop, n, &s[0], nullptr) != CL_SUCCESS) return "";
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

std::string type_name(cl_device_type t) {
    if (t & CL_DEVICE_TYPE_GPU) return "GPU";
    if (t & CL_DEVICE_TYPE_CPU) return "CPU";
    if (t & CL_DEVICE_TYPE_ACCELERATOR) return "ACCELERATOR";
    return "OTHER";
}

// RAII for the selftest resources
struct Resources {
    cl_context ctx = nullptr;
    cl_command_queue q = nullptr;
    cl_program prog = nullptr;
    cl_kernel k = nullptr;
    cl_mem a = nullptr, b = nullptr, c = nullptr;
    ~Resources() {
        if (a) clReleaseMemObject(a);
        if (b) clReleaseMemObject(b);
        if (c) clReleaseMemObject(c);
        if (k) clReleaseKernel(k);
        if (prog) clReleaseProgram(prog);
        if (q) clReleaseCommandQueue(q);
        if (ctx) clReleaseContext(ctx);
    }
};

const char* kVectorAdd =
    "__kernel void vadd(__global const float* a, __global const float* b,"
    "                   __global float* c) {\n"
    "    int i = get_global_id(0);\n"
    "    c[i] = a[i] + b[i];\n"
    "}\n";

}  // namespace

bool available() { return !all_devices().empty(); }

std::vector<DeviceInfo> list_devices() {
    std::vector<DeviceInfo> out;
    for (const DeviceRef& d : all_devices()) {
        DeviceInfo i;
        i.name = dev_string(d.device, CL_DEVICE_NAME);
        i.vendor = dev_string(d.device, CL_DEVICE_VENDOR);
        i.version = dev_string(d.device, CL_DEVICE_VERSION);
        i.driver = dev_string(d.device, CL_DRIVER_VERSION);
        i.type = type_name(d.type);
        cl_ulong mem = 0;
        if (clGetDeviceInfo(d.device, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof mem, &mem, nullptr) == CL_SUCCESS)
            i.global_mem_bytes = (unsigned long long)mem;
        size_t wg = 0;
        if (clGetDeviceInfo(d.device, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof wg, &wg, nullptr) == CL_SUCCESS)
            i.max_work_group = wg;
        i.fp16 = dev_string(d.device, CL_DEVICE_EXTENSIONS).find("cl_khr_fp16") != std::string::npos;
        cl_device_fp_config fp = 0;
        if (clGetDeviceInfo(d.device, CL_DEVICE_DOUBLE_FP_CONFIG, sizeof fp, &fp, nullptr) == CL_SUCCESS)
            i.fp64 = fp != 0;
        out.push_back(std::move(i));
    }
    return out;
}

bool run_vector_add(const std::string& kernel_src, std::string& err) {
    err.clear();
    std::vector<DeviceRef> devs = all_devices();
    if (devs.empty()) {
        err = "no OpenCL devices available";
        return false;
    }
    DeviceRef pick = devs.front();
    for (const DeviceRef& d : devs)
        if (d.type & CL_DEVICE_TYPE_GPU) { pick = d; break; }

    Resources r;
    cl_int rc = CL_SUCCESS;
    r.ctx = clCreateContext(nullptr, 1, &pick.device, nullptr, nullptr, &rc);
    if (!r.ctx) { err = "clCreateContext failed (" + std::to_string(rc) + ")"; return false; }
    r.q = clCreateCommandQueue(r.ctx, pick.device, 0, &rc);
    if (!r.q) { err = "clCreateCommandQueue failed (" + std::to_string(rc) + ")"; return false; }

    const char* src = kernel_src.c_str();
    size_t len = kernel_src.size();
    r.prog = clCreateProgramWithSource(r.ctx, 1, &src, &len, &rc);
    if (!r.prog) { err = "clCreateProgramWithSource failed (" + std::to_string(rc) + ")"; return false; }
    rc = clBuildProgram(r.prog, 1, &pick.device, "", nullptr, nullptr);
    if (rc != CL_SUCCESS) {
        size_t n = 0;
        clGetProgramBuildInfo(r.prog, pick.device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &n);
        std::string log(n, '\0');
        if (n) clGetProgramBuildInfo(r.prog, pick.device, CL_PROGRAM_BUILD_LOG, n, &log[0], nullptr);
        while (!log.empty() && log.back() == '\0') log.pop_back();
        err = "kernel build failed (" + std::to_string(rc) + "): " + log.substr(0, 400);
        return false;
    }
    r.k = clCreateKernel(r.prog, "vadd", &rc);
    if (!r.k) { err = "clCreateKernel(vadd) failed (" + std::to_string(rc) + ")"; return false; }

    const size_t N = 1024;
    std::vector<float> A(N), B(N), C(N, 0.f);
    for (size_t i = 0; i < N; ++i) {
        A[i] = (float)i * 0.5f;
        B[i] = (float)((i * 7) % 31) - 5.f;
    }
    r.a = clCreateBuffer(r.ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N * sizeof(float), A.data(), &rc);
    if (!r.a) { err = "buffer a failed (" + std::to_string(rc) + ")"; return false; }
    r.b = clCreateBuffer(r.ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, N * sizeof(float), B.data(), &rc);
    if (!r.b) { err = "buffer b failed (" + std::to_string(rc) + ")"; return false; }
    r.c = clCreateBuffer(r.ctx, CL_MEM_WRITE_ONLY, N * sizeof(float), nullptr, &rc);
    if (!r.c) { err = "buffer c failed (" + std::to_string(rc) + ")"; return false; }

    if (clSetKernelArg(r.k, 0, sizeof(cl_mem), &r.a) != CL_SUCCESS ||
        clSetKernelArg(r.k, 1, sizeof(cl_mem), &r.b) != CL_SUCCESS ||
        clSetKernelArg(r.k, 2, sizeof(cl_mem), &r.c) != CL_SUCCESS) {
        err = "clSetKernelArg failed";
        return false;
    }
    size_t global = N;
    if (clEnqueueNDRangeKernel(r.q, r.k, 1, nullptr, &global, nullptr, 0, nullptr, nullptr) != CL_SUCCESS) {
        err = "clEnqueueNDRangeKernel failed";
        return false;
    }
    if (clEnqueueReadBuffer(r.q, r.c, CL_TRUE, 0, N * sizeof(float), C.data(), 0, nullptr, nullptr) != CL_SUCCESS) {
        err = "clEnqueueReadBuffer failed";
        return false;
    }
    for (size_t i = 0; i < N; ++i) {
        if (std::fabs(C[i] - (A[i] + B[i])) > 1e-6f) {
            std::ostringstream os;
            os << "wrong result at " << i << ": " << C[i] << " != " << (A[i] + B[i]);
            err = os.str();
            return false;
        }
    }
    return true;
}


// ------------------------------------------------------- lexical similarity
//
// Multi-device engine: every fp64-capable device (GPUs first, then CPU
// devices) gets its own context/queue/program. A tiny calibration measures
// each device's throughput; work is split proportional to speed, and all
// devices are enqueued before any read-back so they run concurrently.
namespace {

struct DevEngine {
    cl_device_id dev = nullptr;
    std::string name;
    cl_context ctx = nullptr;
    cl_command_queue q = nullptr;
    cl_program prog = nullptr;
    cl_kernel k_pairs = nullptr;
    cl_kernel k_matrix = nullptr;
    double speed = 1000.0;   // calibrated pairs/s (floor keeps broken calib usable)
    bool native = false;     // ordinary C++ CPU threads (runs concurrently with GPU)
};

struct SimEngine {
    bool ok = false;
    std::string err;
    std::vector<DevEngine> engines;
};

const char* kSimSource = R"CLC(
#pragma OPENCL EXTENSION cl_khr_fp64 : enable
#define SIM_MAXLEN 256

// Helpers receive private copies of the strings. On CL2.0+ they use the
// generic address space; on CL1.2 (Apple OpenCL) pointers default to
// __private, so SIM_CL12 builds use the private arrays only (which is all
// sim_pair passes after the copy).
#ifdef SIM_CL12
#define SIM_PTR __private const uchar*
#else
#define SIM_PTR const uchar*
#endif

static int tok_count(SIM_PTR s, int off, int len) {
    int n = 1;
    for (int i = 0; i < len; ++i) if (s[off + i] == (uchar)' ') ++n;
    return n;
}
static int tok_off_at(SIM_PTR s, int off, int len, int idx, int* out_len) {
    int cur = 0, start = 0;
    for (int i = 0; i <= len; ++i) {
        if (i == len || s[off + i] == (uchar)' ') {
            if (cur == idx) { *out_len = i - start; return off + start; }
            start = i + 1; ++cur;
        }
    }
    *out_len = 0;
    return off;
}
static int tok_cmp(SIM_PTR A, int ao, int al, SIM_PTR B, int bo, int bl) {
    int m = al < bl ? al : bl;
    for (int k = 0; k < m; ++k)
        if (A[ao + k] != B[bo + k]) return A[ao + k] < B[bo + k] ? -1 : 1;
    if (al == bl) return 0;
    return al < bl ? -1 : 1;
}
static int tok_distinct(SIM_PTR s, int off, int len, int nt) {
    if (nt <= 1) return nt;
    int d = 1, l1 = 0, l2 = 0;
    int o1 = tok_off_at(s, off, len, 0, &l1);
    for (int i = 1; i < nt; ++i) {
        int o2 = tok_off_at(s, off, len, i, &l2);
        if (tok_cmp(s, o1, l1, s, o2, l2) != 0) ++d;
        o1 = o2; l1 = l2;
    }
    return d;
}
static int tokens_sorted(SIM_PTR s, int off, int len, int nt) {
    int l1 = 0, l2 = 0;
    int o1 = tok_off_at(s, off, len, 0, &l1);
    for (int i = 1; i < nt; ++i) {
        int o2 = tok_off_at(s, off, len, i, &l2);
        if (tok_cmp(s, o1, l1, s, o2, l2) > 0) return 0;
        o1 = o2; l1 = l2;
    }
    return 1;
}

// returns the sim_keys score, or -1.0 to ask the host to recompute on CPU
static double sim_pair(__global const uchar* kbuf, int ka, int kl,
                       __global const uchar* cbuf, int ca, int cl) {
    if (kl == 0 || cl == 0) return 0.0;
    if (kl == cl) {
        int eq = 1;
        for (int j = 0; j < kl; ++j)
            if (kbuf[ka + j] != cbuf[ca + j]) { eq = 0; break; }
        if (eq) return 1.0;
    }
    if (kl > SIM_MAXLEN || cl > SIM_MAXLEN) return -1.0;

    // copy into private (aligned) memory before the DP/token loops: avoids
    // unaligned dual-stream global loads that misbehave on some runtimes
    uchar kb[SIM_MAXLEN];
    uchar cb[SIM_MAXLEN];
    for (int i = 0; i < kl; ++i) kb[i] = kbuf[ka + i];
    for (int i = 0; i < cl; ++i) cb[i] = cbuf[ca + i];

    int na = tok_count(kb, 0, kl);
    int nb = tok_count(cb, 0, cl);
    if (!tokens_sorted(kb, 0, kl, na) || !tokens_sorted(cb, 0, cl, nb))
        return -1.0;
    int da = tok_distinct(kb, 0, kl, na);
    int db = tok_distinct(cb, 0, cl, nb);

    int ia = 0, ib = 0, inter = 0, la = 0, lb = 0;
    int oa = tok_off_at(kb, 0, kl, 0, &la);
    int ob = tok_off_at(cb, 0, cl, 0, &lb);
    while (ia < na && ib < nb) {
        int c = tok_cmp(kb, oa, la, cb, ob, lb);
        if (c == 0) {
            ++inter;
            int mo = oa, ml = la;
            do {
                ++ia;
                if (ia < na) oa = tok_off_at(kb, 0, kl, ia, &la);
            } while (ia < na && tok_cmp(kb, oa, la, kb, mo, ml) == 0);
            do {
                ++ib;
                if (ib < nb) ob = tok_off_at(cb, 0, cl, ib, &lb);
            } while (ib < nb && tok_cmp(cb, ob, lb, cb, mo, ml) == 0);
        } else if (c < 0) {
            ++ia;
            if (ia < na) oa = tok_off_at(kb, 0, kl, ia, &la);
        } else {
            ++ib;
            if (ib < nb) ob = tok_off_at(cb, 0, cl, ib, &lb);
        }
    }
    double jac = (da + db - inter) > 0
        ? (double)inter / (double)(da + db - inter) : 0.0;

    // classic single-array rolling LCS
    int dp[SIM_MAXLEN + 1];
    for (int b = 0; b <= cl; ++b) dp[b] = 0;
    for (int a = 1; a <= kl; ++a) {
        int diag = 0;
        uchar ch = kb[a - 1];
        for (int b = 1; b <= cl; ++b) {
            int above = dp[b];
            dp[b] = (ch == cb[b - 1]) ? diag + 1
                    : (above > dp[b - 1] ? above : dp[b - 1]);
            diag = above;
        }
    }
    double lr = 2.0 * (double)dp[cl] / (double)(kl + cl);
    return lr > jac ? lr : jac;
}

__kernel void sim_pairs(
    __global const uchar* kbuf, __global const int* koff, __global const int* klen,
    __global const uchar* cbuf, __global const int* coff, __global const int* clen,
    __global double* out, const int n)
{
    int i = get_global_id(0);
    if (i >= n) return;
    out[i] = sim_pair(kbuf, koff[i], klen[i], cbuf, coff[i], clen[i]);
}

__kernel void sim_matrix(
    __global const uchar* kbuf, __global const int* koff, __global const int* klen, const int nkeys,
    __global const uchar* cbuf, __global const int* coff, __global const int* clen, const int ncands,
    __global double* out, const int total)
{
    int i = get_global_id(0);
    if (i >= total) return;
    int ki = i / ncands;
    int ci = i % ncands;
    out[i] = sim_pair(kbuf, koff[ki], klen[ki], cbuf, coff[ci], clen[ci]);
}
)CLC";

std::once_flag g_sim_once;
SimEngine g_sim;

bool build_engine(const DeviceRef& pick, DevEngine& e, std::string& err) {
    e.dev = pick.device;
    e.name = dev_string(e.dev, CL_DEVICE_NAME);
    cl_int rc = CL_SUCCESS;
    e.ctx = clCreateContext(nullptr, 1, &e.dev, nullptr, nullptr, &rc);
    if (!e.ctx) { err = "context failed (" + std::to_string(rc) + ")"; return false; }
    cl_command_queue_properties qflags =
        std::getenv("ONION_GPU_DEBUG") ? CL_QUEUE_PROFILING_ENABLE : 0;
    e.q = clCreateCommandQueue(e.ctx, e.dev, qflags, &rc);
    if (!e.q) { err = "queue failed (" + std::to_string(rc) + ")"; return false; }
    const char* src = kSimSource;
    size_t len = std::strlen(src);
    e.prog = clCreateProgramWithSource(e.ctx, 1, &src, &len, &rc);
    if (!e.prog) { err = "program failed (" + std::to_string(rc) + ")"; return false; }
    rc = clBuildProgram(e.prog, 1, &e.dev, "-cl-std=CL3.0", nullptr, nullptr);
    if (rc != CL_SUCCESS)
        rc = clBuildProgram(e.prog, 1, &e.dev, "-DSIM_CL12", nullptr, nullptr);
    if (rc != CL_SUCCESS) {
        size_t n = 0;
        clGetProgramBuildInfo(e.prog, e.dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &n);
        std::string log(n, '\0');
        if (n) clGetProgramBuildInfo(e.prog, e.dev, CL_PROGRAM_BUILD_LOG, n, &log[0], nullptr);
        while (!log.empty() && log.back() == '\0') log.pop_back();
        err = "build failed: " + log.substr(0, 220);
        return false;
    }
    e.k_pairs = clCreateKernel(e.prog, "sim_pairs", &rc);
    e.k_matrix = clCreateKernel(e.prog, "sim_matrix", &rc);
    if (!e.k_pairs || !e.k_matrix) { err = "kernel lookup failed"; return false; }
    return true;
}

struct Packed {
    std::vector<unsigned char> bytes;
    std::vector<int> off, len;
};

Packed pack_strings(const std::vector<std::string>& v) {
    Packed p;
    p.off.reserve(v.size());
    p.len.reserve(v.size());
    size_t total = 0;
    for (const auto& s : v) total += s.size();
    p.bytes.reserve(total ? total : 1);
    for (const auto& s : v) {
        p.off.push_back((int)p.bytes.size());
        p.len.push_back((int)s.size());
        p.bytes.insert(p.bytes.end(), s.begin(), s.end());
    }
    return p;
}

cl_mem make_buf(cl_context ctx, const std::vector<unsigned char>& v, cl_mem_flags flags) {
    cl_int rc = CL_SUCCESS;
    if (v.empty()) {
        unsigned char dummy = 0;
        return clCreateBuffer(ctx, flags | CL_MEM_COPY_HOST_PTR, 1, &dummy, &rc);
    }
    return clCreateBuffer(ctx, flags | CL_MEM_COPY_HOST_PTR, v.size(), (void*)v.data(), &rc);
}

cl_mem make_int_buf(cl_context ctx, const std::vector<int>& v) {
    cl_int rc = CL_SUCCESS;
    return clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                          v.size() * sizeof(int), (void*)v.data(), &rc);
}

// split [0,total) across engines proportional to calibrated speed
void split_by_speed(size_t total, std::vector<size_t>& b0, std::vector<size_t>& b1) {
    const size_t nE = g_sim.engines.size();
    b0.assign(nE, 0);
    b1.assign(nE, 0);
    if (nE == 0 || total == 0) return;
    if (nE == 1) { b0[0] = 0; b1[0] = total; return; }
    if (total <= nE) {
        for (size_t i = 0; i < total; ++i) { b0[i] = i; b1[i] = i + 1; }
        return;
    }
    double total_sp = 0.0;
    for (const DevEngine& e : g_sim.engines) total_sp += e.speed;
    size_t cur = 0;
    double spent = 0.0;
    for (size_t i = 0; i < nE; ++i) {
        size_t left_engines = nE - i;
        size_t remaining = total - cur;
        size_t take;
        if (left_engines == 1) {
            take = remaining;
        } else {
            double denom = total_sp - spent;
            double frac = denom > 0 ? g_sim.engines[i].speed / denom : 1.0 / (double)left_engines;
            take = (size_t)std::llround((double)remaining * frac);
            if (take < 1) take = 1;
            size_t max_take = remaining - (left_engines - 1);
            if (take > max_take) take = max_take;
        }
        b0[i] = cur;
        b1[i] = cur + take;
        cur += take;
        spent += g_sim.engines[i].speed;
    }
}

std::vector<std::string> make_synth(int n, int seed) {
    std::vector<std::string> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        std::vector<std::string> t;
        for (int w = 0; w < 3 + (i % 4); ++w)
            t.push_back("w" + std::to_string((i * 7 + w * 13 + seed) % 97));
        std::sort(t.begin(), t.end());
        std::string s;
        for (size_t k = 0; k < t.size(); ++k) { if (k) s += ' '; s += t[k]; }
        v.push_back(s);
    }
    return v;
}

unsigned native_threads() {
    unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    if (const char* e = std::getenv("ONION_CPU_THREADS")) {
        int v = std::atoi(e);
        if (v >= 0) return (unsigned)v;
    }
    return hw;
}

// ordinary C++ CPU threads: NOT serialized with GPU kernels, so this gives
// real CPU+GPU concurrency even on runtimes that serialize OpenCL devices
std::vector<std::string> make_titles(int n, unsigned seed) {
    static const char* W[] = {"alpha","beta","gamma","delta","epsilon","zeta",
        "eta","theta","kappa","lambda","mu","nu","project","charter","risk",
        "plan","scope","review","data","model","system","process","budget",
        "stakeholder","policy","audit","initiation","governance"};
    const int NW = 28;
    std::mt19937 rng(seed);
    auto can = [](std::string t) {
        std::vector<std::string> toks;
        std::string cur;
        for (char c : t) {
            if (c == ' ') { if (!cur.empty()) toks.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) toks.push_back(cur);
        std::sort(toks.begin(), toks.end());
        std::string o;
        for (size_t i = 0; i < toks.size(); ++i) { if (i) o += ' '; o += toks[i]; }
        return o;
    };
    std::vector<std::string> v;
    v.reserve(n);
    for (int i = 0; i < n; ++i) {
        int words = 2 + (int)(rng() % 7);
        std::string t;
        for (int w = 0; w < words; ++w) {
            if (w) t += ' ';
            t += W[rng() % NW];
        }
        v.push_back(can(t));
    }
    return v;
}

void calibrate_native(DevEngine& e) {
    std::vector<std::string> keys = make_titles(512, 1), cands = make_titles(512, 9);
    unsigned nt = native_threads();
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; ++t) {
        th.emplace_back([&, t] {
            double sink = 0.0;
            for (size_t i = t; i < keys.size(); i += nt)
                for (size_t j = 0; j < cands.size(); ++j)
                    sink += depot::sim_keys(keys[i], cands[j]);
            if (sink < -1e300) std::fprintf(stderr, " ");
        });
    }
    for (auto& x : th) x.join();
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double pairs = (double)keys.size() * (double)cands.size();
    e.native = true;
    e.name = "CPU threads (" + std::to_string(nt) + ")";
    e.speed = secs > 0 ? pairs / secs : 1000.0;
}

// small synthetic canonical workload to rank devices by throughput
void calibrate(DevEngine& e) {
    std::vector<std::string> keys = make_synth(128, 0), cands = make_synth(1024, 5);
    Packed kp = pack_strings(keys), cp = pack_strings(cands);
    cl_mem kb = make_buf(e.ctx, kp.bytes, CL_MEM_READ_ONLY);
    cl_mem ko = make_int_buf(e.ctx, kp.off);
    cl_mem kl = make_int_buf(e.ctx, kp.len);
    cl_mem cb = make_buf(e.ctx, cp.bytes, CL_MEM_READ_ONLY);
    cl_mem co = make_int_buf(e.ctx, cp.off);
    cl_mem cl = make_int_buf(e.ctx, cp.len);
    int total = 128 * 1024;
    cl_mem ob = clCreateBuffer(e.ctx, CL_MEM_WRITE_ONLY, (size_t)total * sizeof(double), nullptr, nullptr);
    double best = 0.0;
    if (kb && ko && kl && cb && co && cl && ob) {
        int nk = 128, nc = 1024;
        bool ok = clSetKernelArg(e.k_matrix, 0, sizeof(cl_mem), &kb) == CL_SUCCESS &&
                  clSetKernelArg(e.k_matrix, 1, sizeof(cl_mem), &ko) == CL_SUCCESS &&
                  clSetKernelArg(e.k_matrix, 2, sizeof(cl_mem), &kl) == CL_SUCCESS &&
                  clSetKernelArg(e.k_matrix, 3, sizeof(int), &nk) == CL_SUCCESS &&
                  clSetKernelArg(e.k_matrix, 4, sizeof(cl_mem), &cb) == CL_SUCCESS &&
                  clSetKernelArg(e.k_matrix, 5, sizeof(cl_mem), &co) == CL_SUCCESS &&
                  clSetKernelArg(e.k_matrix, 6, sizeof(cl_mem), &cl) == CL_SUCCESS &&
                  clSetKernelArg(e.k_matrix, 7, sizeof(int), &nc) == CL_SUCCESS &&
                  clSetKernelArg(e.k_matrix, 8, sizeof(cl_mem), &ob) == CL_SUCCESS &&
                  clSetKernelArg(e.k_matrix, 9, sizeof(int), &total) == CL_SUCCESS;
        // 3 reps; the first is warm-up (turbo/alloc effects), the sustained
        // speed is the average of the last two so the split reflects steady
        // state rather than a short burst
        double acc = 0.0;
        int used = 0;
        for (int rep = 0; rep < 3 && ok; ++rep) {
            auto t0 = std::chrono::steady_clock::now();
            size_t gsz = (size_t)total;
            ok = clEnqueueNDRangeKernel(e.q, e.k_matrix, 1, nullptr, &gsz, nullptr, 0, nullptr, nullptr) == CL_SUCCESS &&
                 clFinish(e.q) == CL_SUCCESS;
            double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (ok && secs > 0 && rep > 0) { acc += (double)total / secs; ++used; }
        }
        if (used > 0) best = acc / used;
    }
    if (kb) clReleaseMemObject(kb);
    if (ko) clReleaseMemObject(ko);
    if (kl) clReleaseMemObject(kl);
    if (cb) clReleaseMemObject(cb);
    if (co) clReleaseMemObject(co);
    if (cl) clReleaseMemObject(cl);
    if (ob) clReleaseMemObject(ob);
    if (best > 1000.0) e.speed = best;
}

void sim_init() {
    auto fail = [&](const std::string& why) { g_sim.err = why; g_sim.ok = false; };
    std::vector<DeviceRef> devs = all_devices();
    if (devs.empty()) { fail("no OpenCL devices"); return; }
    auto fp64 = [](cl_device_id d) {
        cl_device_fp_config fp = 0;
        return clGetDeviceInfo(d, CL_DEVICE_DOUBLE_FP_CONFIG, sizeof fp, &fp, nullptr) == CL_SUCCESS && fp != 0;
    };
    std::vector<DeviceRef> order;
    for (const DeviceRef& d : devs)
        if ((d.type & CL_DEVICE_TYPE_GPU) && fp64(d.device)) order.push_back(d);
    for (const DeviceRef& d : devs)
        if (!(d.type & CL_DEVICE_TYPE_GPU) && fp64(d.device)) order.push_back(d);
    if (order.empty()) { fail("no fp64-capable OpenCL device"); return; }
    if (const char* only = std::getenv("ONION_GPU_ONLY")) {   // debug/test: pin one device
        int idx = std::atoi(only);
        if (idx >= 0 && idx < (int)order.size()) order = {order[idx]};
    }
    // Apple's OpenCL runtime serializes kernels across devices in one process
    // (verified with profiling timestamps), so one CL device + native CPU
    // threads beats a multi-device split there.
    if (!order.empty()) {
        char buf[128] = {0};
        clGetPlatformInfo(order[0].platform, CL_PLATFORM_VENDOR, sizeof buf, buf, nullptr);
        if (std::string(buf).find("Apple") != std::string::npos && order.size() > 1)
            order.resize(1);
    }
    std::string notes;
    for (const DeviceRef& d : order) {
        DevEngine e;
        std::string err;
        if (!build_engine(d, e, err)) {
            notes += " [skip " + dev_string(d.device, CL_DEVICE_NAME) + ": " + err + "]";
            continue;
        }
        calibrate(e);
        g_sim.engines.push_back(std::move(e));
    }
    if (g_sim.engines.empty()) { fail("no usable OpenCL device" + notes); return; }
    if (native_threads() > 0) {
        DevEngine cpu;
        calibrate_native(cpu);
        g_sim.engines.push_back(std::move(cpu));
    }
    if (!notes.empty()) g_sim.err = notes;   // informational only
    g_sim.ok = true;
}

}  // namespace

std::string sim_device_name() {
    std::call_once(g_sim_once, sim_init);
    if (!g_sim.ok) return "";
    std::string out;
    for (const DevEngine& e : g_sim.engines) {
        if (!out.empty()) out += ", ";
        out += e.name;
    }
    return out;
}

struct SimIndex::Impl {
    std::vector<std::string> cands;
    struct CBufs { cl_mem bytes = nullptr, off = nullptr, len = nullptr; };
    std::vector<CBufs> per_engine;
    ~Impl() {
        for (CBufs& b : per_engine) {
            if (b.bytes) clReleaseMemObject(b.bytes);
            if (b.off) clReleaseMemObject(b.off);
            if (b.len) clReleaseMemObject(b.len);
        }
    }
};

SimIndex::SimIndex() : impl_(new Impl) {}
SimIndex::~SimIndex() = default;
SimIndex::SimIndex(SimIndex&&) noexcept = default;
SimIndex& SimIndex::operator=(SimIndex&&) noexcept = default;
std::size_t SimIndex::size() const { return impl_->cands.size(); }

bool SimIndex::build(const std::vector<std::string>& cands, std::string& err) {
    err.clear();
    std::call_once(g_sim_once, sim_init);
    if (!g_sim.ok) { err = g_sim.err; return false; }
    impl_->per_engine.clear();
    impl_->cands = cands;
    if (cands.empty()) return true;
    Packed p = pack_strings(cands);
    for (const DevEngine& e : g_sim.engines) {
        if (e.native) continue;
        Impl::CBufs b;
        b.bytes = make_buf(e.ctx, p.bytes, CL_MEM_READ_ONLY);
        b.off = make_int_buf(e.ctx, p.off);
        b.len = make_int_buf(e.ctx, p.len);
        if (!b.bytes || !b.off || !b.len) {
            if (b.bytes) clReleaseMemObject(b.bytes);
            if (b.off) clReleaseMemObject(b.off);
            if (b.len) clReleaseMemObject(b.len);
            impl_->per_engine.clear();
            err = "candidate buffer allocation failed";
            return false;
        }
        impl_->per_engine.push_back(b);
    }
    return true;
}

bool SimIndex::scores(const std::vector<std::string>& keys, std::vector<double>& out,
                      std::string& err) const {
    err.clear();
    std::call_once(g_sim_once, sim_init);
    if (!g_sim.ok) { err = g_sim.err; return false; }
    const size_t K = keys.size();
    const size_t N = impl_->cands.size();
    out.assign(K * N, 0.0);
    if (K == 0 || N == 0) return true;

    const size_t PAIR_LIMIT = size_t(1) << 20;
    const size_t keys_per = std::max<size_t>(1, PAIR_LIMIT / N);
    const size_t nE = g_sim.engines.size();
    const bool dbg = std::getenv("ONION_GPU_DEBUG") != nullptr;

    Packed kp = pack_strings(keys);
    std::vector<size_t> k0, k1;
    split_by_speed(K, k0, k1);
    if (dbg)
        for (size_t i = 0; i < nE; ++i)
            std::fprintf(stderr, "[gpu] engine %zu (%.0f pairs/s): keys %zu..%zu\n",
                         i, g_sim.engines[i].speed, k0[i], k1[i]);

    std::vector<cl_mem> kbs(nE, nullptr);
    for (size_t i = 0; i < nE; ++i)
        if (!g_sim.engines[i].native)
            kbs[i] = make_buf(g_sim.engines[i].ctx, kp.bytes, CL_MEM_READ_ONLY);

    struct Pending { size_t eng, out_off, total; cl_mem koc, klc, ob; cl_event ev = nullptr; };
    std::vector<Pending> pend;
    bool ok = true;
    for (size_t i = 0; i < nE && ok; ++i) {
        if (g_sim.engines[i].native) continue;
        if (k1[i] <= k0[i] || !kbs[i]) continue;
        DevEngine& E = g_sim.engines[i];
        const Impl::CBufs& CB = impl_->per_engine[i];
        for (size_t a = k0[i]; a < k1[i] && ok; a += keys_per) {
            size_t kc = std::min(keys_per, k1[i] - a);
            int total = (int)(kc * N);
            cl_mem koc = clCreateBuffer(E.ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        kc * sizeof(int), kp.off.data() + a, nullptr);
            cl_mem klc = clCreateBuffer(E.ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        kc * sizeof(int), kp.len.data() + a, nullptr);
            cl_mem ob = clCreateBuffer(E.ctx, CL_MEM_WRITE_ONLY,
                                       (size_t)total * sizeof(double), nullptr, nullptr);
            int ki = (int)kc, nc = (int)N;
            if (!koc || !klc || !ob) ok = false;
            if (ok)
                ok = clSetKernelArg(E.k_matrix, 0, sizeof(cl_mem), &kbs[i]) == CL_SUCCESS &&
                     clSetKernelArg(E.k_matrix, 1, sizeof(cl_mem), &koc) == CL_SUCCESS &&
                     clSetKernelArg(E.k_matrix, 2, sizeof(cl_mem), &klc) == CL_SUCCESS &&
                     clSetKernelArg(E.k_matrix, 3, sizeof(int), &ki) == CL_SUCCESS &&
                     clSetKernelArg(E.k_matrix, 4, sizeof(cl_mem), &CB.bytes) == CL_SUCCESS &&
                     clSetKernelArg(E.k_matrix, 5, sizeof(cl_mem), &CB.off) == CL_SUCCESS &&
                     clSetKernelArg(E.k_matrix, 6, sizeof(cl_mem), &CB.len) == CL_SUCCESS &&
                     clSetKernelArg(E.k_matrix, 7, sizeof(int), &nc) == CL_SUCCESS &&
                     clSetKernelArg(E.k_matrix, 8, sizeof(cl_mem), &ob) == CL_SUCCESS &&
                     clSetKernelArg(E.k_matrix, 9, sizeof(int), &total) == CL_SUCCESS;
            cl_event ev = nullptr;
            if (ok) {
                size_t gsz = (size_t)total;
                ok = clEnqueueNDRangeKernel(E.q, E.k_matrix, 1, nullptr, &gsz,
                                            nullptr, 0, nullptr, &ev) == CL_SUCCESS;
            }
            if (ok) {
                pend.push_back({i, a * N, (size_t)total, koc, klc, ob, ev});
            } else {
                if (koc) clReleaseMemObject(koc);
                if (klc) clReleaseMemObject(klc);
                if (ob) clReleaseMemObject(ob);
            }
        }
    }
    // native CPU share: START the threads now and join after the GPU reads,
    // so real CPU+GPU overlap happens even on runtimes that begin kernel
    // execution at the first blocking call (Apple OpenCL behaves this way)
    std::vector<std::thread> native_workers;
    for (size_t i = 0; i < nE && ok; ++i) {
        if (!g_sim.engines[i].native || k1[i] <= k0[i]) continue;
        size_t rows = k1[i] - k0[i];
        unsigned nt = native_threads();
        nt = (unsigned)std::max<size_t>(1, std::min<size_t>(nt ? nt : 1, rows));
        for (unsigned t = 0; t < nt; ++t) {
            size_t r0 = k0[i] + rows * t / nt;
            size_t r1 = k0[i] + rows * (t + 1) / nt;
            native_workers.emplace_back([&, r0, r1] {
                for (size_t r = r0; r < r1; ++r)
                    for (size_t j = 0; j < N; ++j)
                        out[r * N + j] = depot::sim_keys(keys[r], impl_->cands[j]);
            });
        }
    }

    std::vector<double> wait_ms(nE, 0.0);
    for (Pending& p : pend) {
        auto t0 = std::chrono::steady_clock::now();
        if (ok)
            ok = clEnqueueReadBuffer(g_sim.engines[p.eng].q, p.ob, CL_TRUE, 0,
                                     p.total * sizeof(double), out.data() + p.out_off,
                                     0, nullptr, nullptr) == CL_SUCCESS;
        wait_ms[p.eng] += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        if (dbg && p.ev) {
            cl_ulong st = 0, en = 0;
            clGetEventProfilingInfo(p.ev, CL_PROFILING_COMMAND_START, sizeof st, &st, nullptr);
            clGetEventProfilingInfo(p.ev, CL_PROFILING_COMMAND_END, sizeof en, &en, nullptr);
            std::fprintf(stderr, "[gpu] engine %zu kernel start %.1f ms end %.1f ms\n",
                         p.eng, (double)st / 1e6, (double)en / 1e6);
        }
        if (p.ev) clReleaseEvent(p.ev);
        clReleaseMemObject(p.koc);
        clReleaseMemObject(p.klc);
        clReleaseMemObject(p.ob);
    }
    for (auto& x : native_workers) x.join();
    if (dbg)
        for (size_t i = 0; i < nE; ++i)
            std::fprintf(stderr, "[gpu] engine %zu read-wait %.1f ms\n", i, wait_ms[i]);
    for (cl_mem m : kbs)
        if (m) clReleaseMemObject(m);
    if (!ok) { err = "similarity kernel launch/read failed"; return false; }

    // CPU fallback for pairs the kernel refused (-1): over-long or unsorted
    for (size_t i = 0; i < K; ++i) {
        for (size_t j = 0; j < N; ++j) {
            double& v = out[i * N + j];
            if (v < 0.0) v = depot::sim_keys(keys[i], impl_->cands[j]);
        }
    }
    return true;
}

bool sim_keys_batch(const std::vector<std::string>& keys,
                    const std::vector<std::string>& cands,
                    std::vector<double>& out, std::string& err) {
    err.clear();
    std::call_once(g_sim_once, sim_init);
    if (!g_sim.ok) { err = g_sim.err; return false; }
    if (keys.size() != cands.size()) { err = "keys/cands size mismatch"; return false; }
    const size_t n = keys.size();
    out.assign(n, 0.0);
    if (n == 0) return true;

    const size_t nE = g_sim.engines.size();
    const size_t PAIR_LIMIT = size_t(1) << 20;
    Packed kp = pack_strings(keys);
    Packed cp = pack_strings(cands);
    std::vector<size_t> p0, p1;
    split_by_speed(n, p0, p1);

    struct Pending { size_t eng, out_off, total; cl_mem koc, klc, coc, clc, ob; };
    std::vector<Pending> pend;
    std::vector<cl_mem> kbytes(nE, nullptr), cbytes(nE, nullptr);
    bool ok = true;
    for (size_t i = 0; i < nE && ok; ++i) {
        if (g_sim.engines[i].native) continue;
        if (p1[i] <= p0[i]) continue;
        DevEngine& E = g_sim.engines[i];
        kbytes[i] = make_buf(E.ctx, kp.bytes, CL_MEM_READ_ONLY);
        cbytes[i] = make_buf(E.ctx, cp.bytes, CL_MEM_READ_ONLY);
        if (!kbytes[i] || !cbytes[i]) { ok = false; break; }
        for (size_t a = p0[i]; a < p1[i] && ok; a += PAIR_LIMIT) {
            size_t pc = std::min(PAIR_LIMIT, p1[i] - a);
            int total = (int)pc;
            cl_mem koc = clCreateBuffer(E.ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        pc * sizeof(int), kp.off.data() + a, nullptr);
            cl_mem klc = clCreateBuffer(E.ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        pc * sizeof(int), kp.len.data() + a, nullptr);
            cl_mem coc = clCreateBuffer(E.ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        pc * sizeof(int), cp.off.data() + a, nullptr);
            cl_mem clc = clCreateBuffer(E.ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                        pc * sizeof(int), cp.len.data() + a, nullptr);
            cl_mem ob = clCreateBuffer(E.ctx, CL_MEM_WRITE_ONLY,
                                       (size_t)pc * sizeof(double), nullptr, nullptr);
            if (!koc || !klc || !coc || !clc || !ob) ok = false;
            if (ok)
                ok = clSetKernelArg(E.k_pairs, 0, sizeof(cl_mem), &kbytes[i]) == CL_SUCCESS &&
                     clSetKernelArg(E.k_pairs, 1, sizeof(cl_mem), &koc) == CL_SUCCESS &&
                     clSetKernelArg(E.k_pairs, 2, sizeof(cl_mem), &klc) == CL_SUCCESS &&
                     clSetKernelArg(E.k_pairs, 3, sizeof(cl_mem), &cbytes[i]) == CL_SUCCESS &&
                     clSetKernelArg(E.k_pairs, 4, sizeof(cl_mem), &coc) == CL_SUCCESS &&
                     clSetKernelArg(E.k_pairs, 5, sizeof(cl_mem), &clc) == CL_SUCCESS &&
                     clSetKernelArg(E.k_pairs, 6, sizeof(cl_mem), &ob) == CL_SUCCESS &&
                     clSetKernelArg(E.k_pairs, 7, sizeof(int), &total) == CL_SUCCESS;
            if (ok) {
                size_t gsz = pc;
                ok = clEnqueueNDRangeKernel(E.q, E.k_pairs, 1, nullptr, &gsz,
                                            nullptr, 0, nullptr, nullptr) == CL_SUCCESS;
            }
            if (ok) {
                pend.push_back({i, a, (size_t)pc, koc, klc, coc, clc, ob});
            } else {
                if (koc) clReleaseMemObject(koc);
                if (klc) clReleaseMemObject(klc);
                if (coc) clReleaseMemObject(coc);
                if (clc) clReleaseMemObject(clc);
                if (ob) clReleaseMemObject(ob);
            }
        }
    }
    // native CPU share: started before the reads so it overlaps GPU work
    std::vector<std::thread> native_workers;
    for (size_t i = 0; i < nE && ok; ++i) {
        if (!g_sim.engines[i].native || p1[i] <= p0[i]) continue;
        size_t cnt = p1[i] - p0[i];
        unsigned nt = native_threads();
        nt = (unsigned)std::max<size_t>(1, std::min<size_t>(nt ? nt : 1, cnt));
        for (unsigned t = 0; t < nt; ++t) {
            size_t r0 = p0[i] + cnt * t / nt;
            size_t r1 = p0[i] + cnt * (t + 1) / nt;
            native_workers.emplace_back([&, r0, r1] {
                for (size_t r = r0; r < r1; ++r)
                    out[r] = depot::sim_keys(keys[r], cands[r]);
            });
        }
    }

    for (Pending& p : pend) {
        if (ok)
            ok = clEnqueueReadBuffer(g_sim.engines[p.eng].q, p.ob, CL_TRUE, 0,
                                     p.total * sizeof(double), out.data() + p.out_off,
                                     0, nullptr, nullptr) == CL_SUCCESS;
        clReleaseMemObject(p.koc);
        clReleaseMemObject(p.klc);
        clReleaseMemObject(p.coc);
        clReleaseMemObject(p.clc);
        clReleaseMemObject(p.ob);
    }
    for (auto& x : native_workers) x.join();
    for (size_t i = 0; i < nE; ++i) {
        if (kbytes[i]) clReleaseMemObject(kbytes[i]);
        if (cbytes[i]) clReleaseMemObject(cbytes[i]);
    }
    if (!ok) { err = "sim_pairs launch/read failed"; return false; }

    for (size_t i = 0; i < n; ++i)
        if (out[i] < 0.0) out[i] = depot::sim_keys(keys[i], cands[i]);
    return true;
}

bool selftest(std::string& err) { return run_vector_add(kVectorAdd, err); }

}  // namespace gpu
}  // namespace depot
