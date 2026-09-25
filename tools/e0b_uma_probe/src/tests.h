// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "common.h"
#include "guest_mem.h"
#include "report.h"
#include "vk_ctx.h"

namespace e0b {

enum class Backend {
    HostBulk,     // VK_EXT_external_memory_host on the canonical memfd mapping (bulk PA chunk)
    HostGva,      // VK_EXT_external_memory_host on a guest VA range (stitched by guest mmaps)
    DmaBufBulk,   // udmabuf over one PA range -> VK_EXT_external_memory_dma_buf
    DmaBufList,   // UDMABUF_CREATE_LIST over scattered PA pieces (stitched)
    DeviceLocal,  // control: ordinary device memory, like shadPS4 today
    VkExport,     // inverted ownership: exportable Vulkan memory mmap'ed into guest VA
    VkHostVisible // control: ordinary HOST_VISIBLE Vulkan memory, CPU view via vkMapMemory only
};
const char* BackendName(Backend b);

/// Imported (or allocated) memory plus the offset at which the requested range starts.
struct Backing {
    Memory mem;
    uint64_t mem_offset = 0;
    int dmabuf_fd = -1; // kept only when the caller asked for it (DMA_BUF_IOCTL_SYNC tests)
    std::string error;
    bool unsupported = false; // failure is a capability answer rather than an error
    bool Ok() const {
        return mem.mem != VK_NULL_HANDLE;
    }
};

/// One exportable Vulkan allocation whose fd is mmap'ed as "guest physical memory".
struct ExportPool {
    Arena arena;
    Memory mem;
    int fd = -1;
    uint8_t* base = nullptr;
    uint64_t size = 0;
    uint64_t next = 0;
    std::string handle_name;
    std::string error;
    bool Ok() const {
        return base != nullptr;
    }
};

struct Probe {
    Probe(Context& c, GuestMemory& g, DmaBufSource& d, Report& rep, const Options& o)
        : ctx(c), gm(g), dbs(d), r(rep), opt(o) {}

    Context& ctx;
    GuestMemory& gm;
    DmaBufSource& dbs;
    Report& r;
    const Options& opt;

    Arena arena_host;
    Arena arena_dmabuf;
    Arena arena_plain;
    std::string arena_host_err;
    std::string arena_dmabuf_err;
    std::string arena_plain_err;
    ExportPool exp;

    uint64_t block = 64 * KiB; // sparse bind granularity (arena alignment)
    uint64_t unit = 64 * KiB;  // test region granularity: max(block, 64 KiB)
    uint64_t piece = 16 * KiB; // guest mapping granularity (PS4 16 KiB pages)

    uint64_t AllocPa(uint64_t size, uint64_t align);
    uint64_t AllocVa(uint64_t size); // block aligned, leaves a guard block after each range

    const Arena* ArenaFor(Backend b, std::string* why) const;
    // bda_flag: allocate with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT (needed for plain BDA
    // buffers; sparse backing memory does not need it, matching shadPS4's EnsureResident()).
    Backing HostCanonical(uint64_t pa, uint64_t len, uint32_t required_bits, bool bda_flag = false);
    Backing HostGva(uint64_t va, uint64_t len, uint32_t required_bits, bool bda_flag = false);
    Backing DmaBufSingle(uint64_t pa, uint64_t len, uint32_t required_bits, bool keep_fd,
                         bool bda_flag = false);
    Backing DmaBufList(const std::vector<std::pair<uint64_t, uint64_t>>& pieces,
                       uint32_t required_bits, bool keep_fd, bool bda_flag = false);
    void Release(Backing& b);

    uint32_t NextSeed() {
        return seed_state_ = seed_state_ * 1664525u + 1013904223u;
    }

    uint64_t AllocExport(uint64_t size); // offset inside exp.mem, unit aligned

    void SetupArenas();
    void SetupExport();
    void DestroyArenas();

private:
    uint64_t pa_next_ = 0;
    uint64_t va_next_ = 0;
    uint32_t seed_state_ = 0;
    friend void InitProbe(Probe& p);
};

void InitProbe(Probe& p);
void CollectEnvironment(Report& r);
void RunCapabilities(Probe& p);
void RunFunctional(Probe& p);

} // namespace e0b
