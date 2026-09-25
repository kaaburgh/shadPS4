// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

namespace e0b {

constexpr uint64_t KiB = 1024;
constexpr uint64_t MiB = 1024 * KiB;
constexpr uint64_t GiB = 1024 * MiB;

// Shader modes, keep in sync with shaders/*.comp.
constexpr uint32_t MODE_VERIFY = 1;
constexpr uint32_t MODE_WRITE = 2;
constexpr uint32_t MODE_NONZERO = 4;

// Same hash as pattern() in the shaders.
inline uint32_t Pattern(uint32_t i, uint32_t seed) {
    uint32_t x = i * 0x9E3779B9u + seed;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

struct CpuCheck {
    uint64_t mismatches = 0;
    int64_t first_bad = -1;
    uint32_t first_value = 0;
};

// Writes pattern(index_base + i, seed) for i in [0, count) using volatile stores so
// the compiler cannot elide or reorder them across the submit that follows.
void CpuWrite(void* dst, uint64_t count, uint32_t seed, uint32_t index_base = 0);
CpuCheck CpuVerify(const void* src, uint64_t count, uint32_t seed, uint32_t index_base = 0);

struct VkError : std::runtime_error {
    VkResult result;
    VkError(VkResult r, const std::string& what) : std::runtime_error(what), result(r) {}
};

// A test cannot run on this device/configuration; reported as UNSUPPORTED.
struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};

const char* ResultName(VkResult r);
std::string Hex(uint64_t v);
std::string TypeBits(uint32_t bits);
std::string Sprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

#define E0B_CHECK(expr)                                                                            \
    do {                                                                                           \
        const VkResult e0b_r_ = (expr);                                                            \
        if (e0b_r_ != VK_SUCCESS) {                                                                \
            throw ::e0b::VkError(e0b_r_, std::string(#expr) + " -> " + ::e0b::ResultName(e0b_r_)); \
        }                                                                                          \
    } while (0)

class Defer {
public:
    explicit Defer(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~Defer() {
        if (fn_) {
            fn_();
        }
    }
    Defer(const Defer&) = delete;
    Defer& operator=(const Defer&) = delete;

private:
    std::function<void()> fn_;
};

inline double NowUs() {
    using namespace std::chrono;
    return duration<double, std::micro>(steady_clock::now().time_since_epoch()).count();
}

double Median(std::vector<double> v);

struct Options {
    bool list_devices = false;
    int device_index = -1;
    std::string device_name;
    bool allow_cpu = false;
    bool validate = false;
    std::string json_path;
    uint64_t backing_mib = 512;
    uint64_t arena_mib = 1024;
    std::set<std::string> tests; // empty -> all
    uint32_t t4_iters = 256;
    uint32_t t6_max = 4096;
    uint32_t t6_iters = 64;
    bool fake_dmabuf = false;
    bool force_type = false;
    uint32_t seed = 0x5EED;

    bool Enabled(const std::string& name) const {
        return tests.empty() || tests.count(name) != 0;
    }
};

} // namespace e0b
