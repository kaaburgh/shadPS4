// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Helpers shared by the functional tests (functional.cpp) and the plan-B tests (planb.cpp).

#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "tests.h"

namespace e0b {

/// Accumulates named mismatch counts from GPU result slots and CPU verifications.
struct Checks {
    struct Item {
        std::string name;
        uint64_t mismatches;
        std::string note;
    };
    std::vector<Item> items;

    void Gpu(const std::string& name, const SlotResult& s, uint32_t expected) {
        std::string note;
        uint64_t bad = s.mismatches;
        if (s.checked != expected) {
            // A dispatch that did not run (or ran partially) would otherwise look clean.
            note = Sprintf("checked %u of %u", s.checked, expected);
            bad += expected > s.checked ? expected - s.checked : 1;
        }
        if (s.mismatches) {
            note += Sprintf("%sfirst bad dword %u = %s", note.empty() ? "" : ", ", s.first_bad,
                            Hex(s.first_value).c_str());
        }
        items.push_back({name, bad, note});
    }
    void Cpu(const std::string& name, const CpuCheck& c) {
        std::string note;
        if (c.mismatches) {
            note = Sprintf("first bad dword %lld = %s", static_cast<long long>(c.first_bad),
                           Hex(c.first_value).c_str());
        }
        items.push_back({name, c.mismatches, note});
    }
    /// Records a raw count that must be zero.
    void Count(const std::string& name, uint64_t bad, const std::string& note = {}) {
        items.push_back({name, bad, note});
    }
    bool Clean() const {
        return std::all_of(items.begin(), items.end(),
                           [](const Item& i) { return i.mismatches == 0; });
    }
    std::string Detail() const {
        std::string s;
        for (const auto& i : items) {
            s += (s.empty() ? "" : "; ") + i.name + " " + std::to_string(i.mismatches);
            if (!i.note.empty()) {
                s += " (" + i.note + ")";
            }
        }
        return s;
    }
};

/// Runs one test variant and turns exceptions into report entries.
template <class F>
void RunVariant(Probe& p, const std::string& test, const std::string& variant, F&& fn) {
    if (p.ctx.device_lost) {
        p.r.Add(test, variant, Status::Skip, "device lost earlier");
        return;
    }
    try {
        fn();
    } catch (const Unsupported& e) {
        p.r.Add(test, variant, Status::Unsupported, e.what());
    } catch (const VkError& e) {
        p.r.Add(test, variant, e.result == VK_ERROR_DEVICE_LOST ? Status::Fail : Status::Error,
                e.what());
    } catch (const std::exception& e) {
        p.r.Add(test, variant, Status::Error, e.what());
    }
}

inline std::vector<Path> Paths(const Probe& p) {
    std::vector<Path> v;
    if (p.ctx.bda_pipe) {
        v.push_back(Path::Bda);
    }
    v.push_back(Path::Ssbo);
    return v;
}

inline Path PreferredPath(const Probe& p) {
    return p.ctx.bda_pipe ? Path::Bda : Path::Ssbo;
}

inline void AddChecks(Probe& p, const std::string& test, const std::string& variant,
                      const Checks& c, const std::string& extra = {}) {
    p.r.Add(test, variant, c.Clean() ? Status::Pass : Status::Fail,
            c.Detail() + (extra.empty() ? "" : " | " + extra));
}

/// Largest object count a scaling test may create. Every import counts against
/// maxMemoryAllocationCount like any allocation, and the probe and the driver already hold
/// others, so keep a wide margin below the limit. Reports the cap as a fact.
inline uint64_t ObjectCap(Probe& p, const std::string& fact) {
    const uint64_t limit = p.ctx.props.limits.maxMemoryAllocationCount;
    const uint64_t margin = std::max<uint64_t>(256, limit / 4);
    const uint64_t alloc_cap = limit > margin ? limit - margin : 0;
    uint64_t cap = std::min<uint64_t>(p.opt.t6_max, alloc_cap);
    if (p.dbs.IsFake()) {
        cap = std::min<uint64_t>(cap, 256); // each fake dma-buf maps the whole memfd
    }
    p.r.Fact(fact,
             Sprintf("%llu (maxMemoryAllocationCount %llu, margin %llu, --t6-max %u)",
                     static_cast<unsigned long long>(cap), static_cast<unsigned long long>(limit),
                     static_cast<unsigned long long>(margin), p.opt.t6_max));
    return cap;
}

/// N values for scaling tests, filtered by `cap`.
inline std::vector<uint32_t> ScaleSteps(uint64_t cap) {
    std::vector<uint32_t> v;
    for (const uint32_t n : {1u, 16u, 128u, 1024u, 2048u, 4096u}) {
        if (n <= cap) {
            v.push_back(n);
        }
    }
    return v;
}

} // namespace e0b
