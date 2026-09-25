// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cerrno>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#include "tests.h"

namespace e0b {

const char* BackendName(Backend b) {
    switch (b) {
    case Backend::HostBulk:
        return "host_bulk";
    case Backend::HostGva:
        return "host_gva";
    case Backend::DmaBufBulk:
        return "dmabuf_bulk";
    case Backend::DmaBufList:
        return "dmabuf_list";
    case Backend::DeviceLocal:
        return "device_local";
    case Backend::VkExport:
        return "vk_export";
    case Backend::VkHostVisible:
        return "vk_hostvisible";
    }
    return "?";
}

void InitProbe(Probe& p) {
    p.seed_state_ = p.opt.seed;
}

uint64_t Probe::AllocPa(uint64_t size, uint64_t align) {
    const uint64_t start = (pa_next_ + align - 1) / align * align;
    if (start + size > gm.Size()) {
        throw std::runtime_error("backing memfd too small, raise --backing-mib");
    }
    pa_next_ = start + size;
    return start;
}

uint64_t Probe::AllocVa(uint64_t size) {
    const uint64_t start = (va_next_ + unit - 1) / unit * unit;
    const uint64_t len = (size + unit - 1) / unit * unit;
    if (start + len > gm.VaSpan()) {
        throw std::runtime_error("arena/guest VA span too small, raise --arena-mib");
    }
    va_next_ = start + len + unit; // guard unit
    return start;
}

void Probe::SetupArenas() {
    const VkDeviceSize size = opt.arena_mib * MiB;
    const bool aliased = ctx.f_sparse_aliased;
    auto make = [&](Arena& a, std::string& err, VkExternalMemoryHandleTypeFlags h) {
        try {
            a = ctx.CreateArena(size, h, aliased);
        } catch (const std::exception& e) {
            err = e.what();
        }
    };
    make(arena_plain, arena_plain_err, 0);
    if (ctx.ext_host) {
        make(arena_host, arena_host_err, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
    } else {
        arena_host_err = "VK_EXT_external_memory_host not available";
    }
    if (ctx.ext_dmabuf) {
        make(arena_dmabuf, arena_dmabuf_err, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    } else {
        arena_dmabuf_err = "VK_EXT_external_memory_dma_buf not available";
    }
    uint64_t a = 0;
    for (const Arena* ar : {&arena_plain, &arena_host, &arena_dmabuf}) {
        if (ar->buf) {
            a = std::max<uint64_t>(a, ar->reqs.alignment);
        }
    }
    block = a ? a : 64 * KiB;
    unit = std::max<uint64_t>(block, 64 * KiB);
    piece = 16 * KiB;
}

void Probe::SetupExport() {
    if (!ctx.ext_fd) {
        exp.error = "VK_KHR_external_memory_fd not available";
        return;
    }
    exp.size = 64 * MiB;
    std::vector<std::pair<VkExternalMemoryHandleTypeFlagBits, const char*>> handles;
    if (ctx.ext_dmabuf) {
        handles.push_back({VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "dma_buf"});
    }
    handles.push_back({VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "opaque_fd"});
    std::string errors;
    for (const auto& [h, name] : handles) {
        Arena a;
        try {
            a = ctx.CreateArena(opt.arena_mib * MiB, h, ctx.f_sparse_aliased);
        } catch (const std::exception& e) {
            errors += std::string(name) + ": arena: " + e.what() + "; ";
            continue;
        }
        int fd = -1;
        Memory m = ctx.AllocateExportable(exp.size, a.reqs.memoryTypeBits, h, &fd);
        if (!m) {
            errors += std::string(name) + ": " + m.note + "; ";
            ctx.Destroy(a);
            continue;
        }
        void* base = mmap(nullptr, exp.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) {
            errors += std::string(name) + ": exported fd is not mmap-able (" +
                      std::strerror(errno) + "); ";
            close(fd);
            ctx.Free(m);
            ctx.Destroy(a);
            continue;
        }
        exp.arena = a;
        exp.mem = m;
        exp.fd = fd;
        exp.base = static_cast<uint8_t*>(base);
        exp.handle_name = name;
        return;
    }
    exp.error = errors;
}

uint64_t Probe::AllocExport(uint64_t size) {
    const uint64_t start = (exp.next + unit - 1) / unit * unit;
    if (start + size > exp.size) {
        throw std::runtime_error("export pool exhausted");
    }
    exp.next = start + size;
    return start;
}

void Probe::DestroyArenas() {
    ctx.Destroy(exp.arena);
    if (exp.base) {
        munmap(exp.base, exp.size);
        exp.base = nullptr;
    }
    ctx.Free(exp.mem);
    if (exp.fd >= 0) {
        close(exp.fd);
        exp.fd = -1;
    }
    ctx.Destroy(arena_plain);
    ctx.Destroy(arena_host);
    ctx.Destroy(arena_dmabuf);
}

const Arena* Probe::ArenaFor(Backend b, std::string* why) const {
    const Arena* a = nullptr;
    switch (b) {
    case Backend::HostBulk:
    case Backend::HostGva:
        a = &arena_host;
        *why = arena_host_err;
        break;
    case Backend::DmaBufBulk:
    case Backend::DmaBufList:
        a = &arena_dmabuf;
        *why = arena_dmabuf_err;
        break;
    case Backend::DeviceLocal:
    case Backend::VkHostVisible:
        a = &arena_plain;
        *why = arena_plain_err;
        break;
    case Backend::VkExport:
        a = exp.Ok() ? &exp.arena : nullptr;
        *why = exp.error;
        break;
    }
    return a && a->buf ? a : nullptr;
}

static Backing FromMemory(Memory m, uint64_t offset) {
    Backing b;
    b.mem = m;
    b.mem_offset = offset;
    if (!m.mem) {
        b.error = m.note.empty() ? ResultName(m.result) : m.note;
        // An import the driver/kernel refuses is a capability answer, not a probe bug.
        b.unsupported = m.result == VK_ERROR_FEATURE_NOT_PRESENT ||
                        m.result == VK_ERROR_EXTENSION_NOT_PRESENT ||
                        m.result == VK_ERROR_INVALID_EXTERNAL_HANDLE ||
                        m.result == VK_ERROR_OUT_OF_DEVICE_MEMORY ||
                        m.result == VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return b;
}

Backing Probe::HostCanonical(uint64_t pa, uint64_t len, uint32_t required_bits, bool bda_flag) {
    return FromMemory(ctx.ImportHost(gm.Canonical(pa), len, required_bits, bda_flag), 0);
}

Backing Probe::HostGva(uint64_t va, uint64_t len, uint32_t required_bits, bool bda_flag) {
    return FromMemory(ctx.ImportHost(gm.Gva(va), len, required_bits, bda_flag), 0);
}

Backing Probe::DmaBufSingle(uint64_t pa, uint64_t len, uint32_t required_bits, bool keep_fd,
                            bool bda_flag) {
    DmaBuf d = dbs.Single(pa, len);
    if (!d) {
        Backing b;
        b.error = d.error;
        b.unsupported = true;
        return b;
    }
    Backing b = FromMemory(ctx.ImportDmaBuf(d.fd, d.size, required_bits, bda_flag), d.offset);
    if (keep_fd && b.Ok()) {
        b.dmabuf_fd = d.fd;
    } else {
        close(d.fd);
    }
    return b;
}

Backing Probe::DmaBufList(const std::vector<std::pair<uint64_t, uint64_t>>& pieces,
                          uint32_t required_bits, bool keep_fd, bool bda_flag) {
    DmaBuf d = dbs.List(pieces);
    if (!d) {
        Backing b;
        b.error = d.error;
        b.unsupported = true;
        return b;
    }
    Backing b = FromMemory(ctx.ImportDmaBuf(d.fd, d.size, required_bits, bda_flag), d.offset);
    if (keep_fd && b.Ok()) {
        b.dmabuf_fd = d.fd;
    } else {
        close(d.fd);
    }
    return b;
}

void Probe::Release(Backing& b) {
    ctx.Free(b.mem);
    if (b.dmabuf_fd >= 0) {
        close(b.dmabuf_fd);
        b.dmabuf_fd = -1;
    }
}

} // namespace e0b
