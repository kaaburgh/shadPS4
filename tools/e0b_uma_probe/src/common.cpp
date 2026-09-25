// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdarg>
#include <cstdio>

#include "common.h"

namespace e0b {

void CpuWrite(void* dst, uint64_t count, uint32_t seed, uint32_t index_base) {
    auto* p = static_cast<volatile uint32_t*>(dst);
    for (uint64_t i = 0; i < count; ++i) {
        p[i] = Pattern(static_cast<uint32_t>(index_base + i), seed);
    }
}

CpuCheck CpuVerify(const void* src, uint64_t count, uint32_t seed, uint32_t index_base) {
    CpuCheck c;
    const auto* p = static_cast<const volatile uint32_t*>(src);
    for (uint64_t i = 0; i < count; ++i) {
        const uint32_t v = p[i];
        if (v != Pattern(static_cast<uint32_t>(index_base + i), seed)) {
            if (c.mismatches == 0) {
                c.first_bad = static_cast<int64_t>(i);
                c.first_value = v;
            }
            ++c.mismatches;
        }
    }
    return c;
}

const char* ResultName(VkResult r) {
    switch (r) {
#define E0B_CASE(x)                                                                                \
    case x:                                                                                        \
        return #x;
        E0B_CASE(VK_SUCCESS)
        E0B_CASE(VK_NOT_READY)
        E0B_CASE(VK_TIMEOUT)
        E0B_CASE(VK_INCOMPLETE)
        E0B_CASE(VK_ERROR_OUT_OF_HOST_MEMORY)
        E0B_CASE(VK_ERROR_OUT_OF_DEVICE_MEMORY)
        E0B_CASE(VK_ERROR_INITIALIZATION_FAILED)
        E0B_CASE(VK_ERROR_DEVICE_LOST)
        E0B_CASE(VK_ERROR_MEMORY_MAP_FAILED)
        E0B_CASE(VK_ERROR_LAYER_NOT_PRESENT)
        E0B_CASE(VK_ERROR_EXTENSION_NOT_PRESENT)
        E0B_CASE(VK_ERROR_FEATURE_NOT_PRESENT)
        E0B_CASE(VK_ERROR_INCOMPATIBLE_DRIVER)
        E0B_CASE(VK_ERROR_TOO_MANY_OBJECTS)
        E0B_CASE(VK_ERROR_FORMAT_NOT_SUPPORTED)
        E0B_CASE(VK_ERROR_FRAGMENTED_POOL)
        E0B_CASE(VK_ERROR_UNKNOWN)
        E0B_CASE(VK_ERROR_OUT_OF_POOL_MEMORY)
        E0B_CASE(VK_ERROR_INVALID_EXTERNAL_HANDLE)
        E0B_CASE(VK_ERROR_FRAGMENTATION)
        E0B_CASE(VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS)
#undef E0B_CASE
    default:
        return "VK_RESULT_UNKNOWN_VALUE";
    }
}

std::string Hex(uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string TypeBits(uint32_t bits) {
    std::string s = Hex(bits) + " {";
    bool first = true;
    for (uint32_t i = 0; i < 32; ++i) {
        if (bits & (1u << i)) {
            s += first ? "" : ",";
            s += std::to_string(i);
            first = false;
        }
    }
    return s + "}";
}

std::string Sprintf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string s(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) {
        std::vsnprintf(s.data(), s.size() + 1, fmt, ap2);
    }
    va_end(ap2);
    return s;
}

double Median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

} // namespace e0b
