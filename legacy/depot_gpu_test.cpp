// depot_gpu_test.cpp — hermetic tests for the OpenCL layer (skips when no
// platform is available, so machines without OpenCL still pass the suite).
#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "depot_core.hpp"
#include "depot_gpu.hpp"

static std::vector<std::string> rand_titles(std::size_t n, unsigned seed) {
    static const char* W[] = {
        "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta",
        "kappa", "lambda", "mu", "nu", "project", "charter", "risk", "plan",
        "scope", "review", "data", "model", "system", "process", "budget",
        "stakeholder", "policy", "audit", "initiation", "governance"};
    const std::size_t NW = sizeof(W) / sizeof(W[0]);
    std::mt19937 rng(seed);
    std::vector<std::string> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        int words = 2 + (int)(rng() % 7);
        std::string t;
        for (int w = 0; w < words; ++w) {
            if (w) t += ' ';
            t += W[rng() % NW];
        }
        out.push_back(depot::canonical(t));
    }
    return out;
}

static bool gpu_ready() { return depot::gpu::available(); }

TEST(Gpu, Enumeration) {
    if (!gpu_ready()) GTEST_SKIP() << "no OpenCL platform on this machine";
    auto devs = depot::gpu::list_devices();
    ASSERT_FALSE(devs.empty());
    for (const auto& d : devs) {
        EXPECT_FALSE(d.name.empty());
        EXPECT_FALSE(d.type.empty());
        EXPECT_GT(d.max_work_group, 0u);
    }
}

TEST(Gpu, VectorAddKnownAnswer) {
    if (!gpu_ready()) GTEST_SKIP() << "no OpenCL platform on this machine";
    std::string err;
    EXPECT_TRUE(depot::gpu::selftest(err)) << err;
}

TEST(Gpu, BrokenKernelFailsCleanly) {
    if (!gpu_ready()) GTEST_SKIP() << "no OpenCL platform on this machine";
    std::string err;
    EXPECT_FALSE(depot::gpu::run_vector_add("__kernel void vadd( {", err));
    EXPECT_FALSE(err.empty());
}

TEST(Gpu, SimParityRandomPairs) {
    if (!gpu_ready()) GTEST_SKIP() << "no OpenCL platform on this machine";
    auto keys = rand_titles(64, 11);
    auto cands = rand_titles(64, 22);
    std::vector<double> gpu;
    std::string err;
    ASSERT_TRUE(depot::gpu::sim_keys_batch(keys, cands, gpu, err)) << err;
    ASSERT_EQ(gpu.size(), keys.size());
    for (std::size_t i = 0; i < keys.size(); ++i) {
        double cpu = depot::sim_keys(keys[i], cands[i]);
        EXPECT_NEAR(gpu[i], cpu, 1e-9) << "pair " << i;
    }
}

TEST(Gpu, SimParityMatrix) {
    if (!gpu_ready()) GTEST_SKIP() << "no OpenCL platform on this machine";
    auto keys = rand_titles(24, 33);
    auto cands = rand_titles(300, 44);
    depot::gpu::SimIndex idx;
    std::string err;
    ASSERT_TRUE(idx.build(cands, err)) << err;
    EXPECT_EQ(idx.size(), cands.size());
    std::vector<double> gpu;
    ASSERT_TRUE(idx.scores(keys, gpu, err)) << err;
    ASSERT_EQ(gpu.size(), keys.size() * cands.size());
    for (std::size_t i = 0; i < keys.size(); ++i)
        for (std::size_t j = 0; j < cands.size(); ++j) {
            double cpu = depot::sim_keys(keys[i], cands[j]);
            EXPECT_NEAR(gpu[i * cands.size() + j], cpu, 1e-9)
                << "key " << i << " cand " << j;
        }
}

TEST(Gpu, SimParityMultiDeviceMatrix) {
    if (!gpu_ready()) GTEST_SKIP() << "no OpenCL platform on this machine";
    auto keys = rand_titles(96, 77);
    auto cands = rand_titles(1200, 88);
    depot::gpu::SimIndex idx;
    std::string err;
    ASSERT_TRUE(idx.build(cands, err)) << err;
    std::vector<double> gpu;
    ASSERT_TRUE(idx.scores(keys, gpu, err)) << err;
    ASSERT_EQ(gpu.size(), keys.size() * cands.size());
    std::size_t bad = 0;
    for (std::size_t i = 0; i < keys.size(); ++i)
        for (std::size_t j = 0; j < cands.size(); ++j)
            if (std::fabs(gpu[i * cands.size() + j] - depot::sim_keys(keys[i], cands[j])) > 1e-9)
                ++bad;
    EXPECT_EQ(bad, 0u);
    EXPECT_FALSE(depot::gpu::sim_device_name().empty());
}

TEST(Gpu, SimBatchMultiDeviceSplit) {
    if (!gpu_ready()) GTEST_SKIP() << "no OpenCL platform on this machine";
    auto keys = rand_titles(8000, 99);
    auto cands = rand_titles(8000, 111);
    std::vector<double> gpu;
    std::string err;
    ASSERT_TRUE(depot::gpu::sim_keys_batch(keys, cands, gpu, err)) << err;
    std::size_t bad = 0;
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (std::fabs(gpu[i] - depot::sim_keys(keys[i], cands[i])) > 1e-9) ++bad;
    EXPECT_EQ(bad, 0u);
}

TEST(Gpu, SimBoundariesAndFallback) {
    if (!gpu_ready()) GTEST_SKIP() << "no OpenCL platform on this machine";
    std::string long_a(400, 'x'), long_b(300, 'x');   // > SIM_MAXLEN -> CPU fallback
    std::vector<std::string> keys = {"", "alpha beta", "zz aa", long_a, "same"};
    std::vector<std::string> cands = {"", "alpha beta", "zz aa", long_b, "same"};
    std::vector<double> gpu;
    std::string err;
    ASSERT_TRUE(depot::gpu::sim_keys_batch(keys, cands, gpu, err)) << err;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        double cpu = depot::sim_keys(keys[i], cands[i]);
        EXPECT_NEAR(gpu[i], cpu, 1e-9) << "boundary pair " << i;
    }
    EXPECT_DOUBLE_EQ(gpu[0], 0.0);   // empty
    EXPECT_DOUBLE_EQ(gpu[1], 1.0);   // equal
    EXPECT_DOUBLE_EQ(gpu[4], 1.0);   // equal
}
