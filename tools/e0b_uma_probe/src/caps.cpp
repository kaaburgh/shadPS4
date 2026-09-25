// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Capability matrix: what the driver says before any data moves. The key line is the
// intersection between a sparse arena's memoryTypeBits and the types an import allows.

#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#include "tests.h"

namespace e0b {

static std::string HandleName(VkExternalMemoryHandleTypeFlags h) {
    if (h == 0) {
        return "none";
    }
    if (h == VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) {
        return "host";
    }
    if (h == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
        return "dmabuf";
    }
    if (h == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT) {
        return "opaque_fd";
    }
    return Hex(h);
}

static std::string ExtFeatures(VkExternalMemoryFeatureFlags f) {
    std::string s;
    if (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
        s += "DEDICATED_ONLY ";
    if (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)
        s += "EXPORTABLE ";
    if (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
        s += "IMPORTABLE ";
    return s.empty() ? "none" : s;
}

static void SparseMatrix(Probe& p) {
    auto& ctx = p.ctx;
    p.r.Section("capabilities: sparse arena memory requirements");
    std::vector<VkExternalMemoryHandleTypeFlags> handles = {0};
    if (ctx.ext_host) {
        handles.push_back(VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
    }
    if (ctx.ext_dmabuf) {
        handles.push_back(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    }
    for (const auto h : handles) {
        for (const bool aliased : {false, true}) {
            for (const VkDeviceSize size :
                 {VkDeviceSize(p.opt.arena_mib * MiB), VkDeviceSize(4 * GiB)}) {
                const std::string key = Sprintf("sparse[%s,%s,%lluMiB]", HandleName(h).c_str(),
                                                aliased ? "aliased" : "plain",
                                                static_cast<unsigned long long>(size / MiB));
                try {
                    Arena a = ctx.CreateArena(size, h, aliased);
                    p.r.Fact(key, "align " + Hex(a.reqs.alignment) + " bits " +
                                      TypeBits(a.reqs.memoryTypeBits));
                    ctx.Destroy(a);
                } catch (const std::exception& e) {
                    p.r.Fact(key, std::string("<") + e.what() + ">");
                }
            }
        }
    }

    p.r.Section("capabilities: vkGetPhysicalDeviceExternalBufferProperties");
    const VkBufferUsageFlags usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     (ctx.f_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
    std::vector<VkExternalMemoryHandleTypeFlagBits> types = {
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT};
    if (ctx.ext_host) {
        types.push_back(VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
    }
    if (ctx.ext_dmabuf) {
        types.push_back(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    }
    const VkBufferCreateFlags sparse =
        VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT;
    for (const auto t : types) {
        for (const VkBufferCreateFlags flags :
             {VkBufferCreateFlags(0), sparse, sparse | VK_BUFFER_CREATE_SPARSE_ALIASED_BIT}) {
            VkPhysicalDeviceExternalBufferInfo info{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
            info.flags = flags;
            info.usage = usage;
            info.handleType = t;
            VkExternalBufferProperties out{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
            vkGetPhysicalDeviceExternalBufferProperties(ctx.phys, &info, &out);
            const auto& mp = out.externalMemoryProperties;
            p.r.Fact(Sprintf("extbuf[%s,%s]", HandleName(t).c_str(),
                             flags == 0                                      ? "plain"
                             : (flags & VK_BUFFER_CREATE_SPARSE_ALIASED_BIT) ? "sparse+aliased"
                                                                             : "sparse"),
                     ExtFeatures(mp.externalMemoryFeatures) + "compatible " +
                         Hex(mp.compatibleHandleTypes));
        }
    }
}

static void HostPointerMatrix(Probe& p) {
    auto& ctx = p.ctx;
    p.r.Section("capabilities: host pointer import (VK_EXT_external_memory_host)");
    if (!ctx.ext_host) {
        p.r.Fact("host_import", "<extension not available>");
        return;
    }
    const uint64_t len = p.unit;
    const uint32_t sparse_bits = p.arena_host.buf ? p.arena_host.reqs.memoryTypeBits : 0;

    struct Source {
        const char* name;
        void* ptr;
    };
    // 2 MiB aligned anonymous buffers, like the canonical mapping, so a large
    // minImportedHostPointerAlignment cannot be the reason an import fails.
    constexpr uint64_t kAlign = 2 * MiB;
    const auto aligned = [](void* r) {
        return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(r) + kAlign - 1) &
                                       ~uintptr_t(kAlign - 1));
    };
    void* priv_res =
        mmap(nullptr, len + kAlign, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* shared_res =
        mmap(nullptr, len + kAlign, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (priv_res == MAP_FAILED || shared_res == MAP_FAILED) {
        throw std::runtime_error("mmap(anonymous test buffers) failed");
    }
    Defer unmap([&] {
        munmap(priv_res, len + kAlign);
        munmap(shared_res, len + kAlign);
    });
    void* anon_private = aligned(priv_res);
    void* anon_shared = aligned(shared_res);
    std::memset(anon_private, 0, len);
    std::memset(anon_shared, 0, len);
    const uint64_t pa = p.AllocPa(len, p.unit);
    const uint64_t va = p.AllocVa(len);
    p.gm.Map(va, pa, len);
    Defer unmap_guest([&] { p.gm.Unmap(va, len); });

    const Source sources[] = {
        {"anon_private", anon_private},
        {"anon_shared(shmem)", anon_shared},
        {"memfd_canonical(backing_base)", p.gm.Canonical(pa)},
        {"memfd_guest_va(alias)", p.gm.Gva(va)},
    };
    for (const auto& s : sources) {
        if (!ctx.HostImportAligned(s.ptr, len)) {
            p.r.Fact(std::string("host[") + s.name + "]",
                     Sprintf("<skipped: pointer %s / size %s not a multiple of "
                             "minImportedHostPointerAlignment %s>",
                             Hex(reinterpret_cast<uintptr_t>(s.ptr)).c_str(), Hex(len).c_str(),
                             Hex(ctx.min_host_ptr_align).c_str()));
            continue;
        }
        VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        const VkResult pr = ctx.GetHostPtrProps(
            ctx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, s.ptr, &hp);
        std::string v =
            Sprintf("props %s bits %s", ResultName(pr), TypeBits(hp.memoryTypeBits).c_str());
        // Trial import with any allowed type: answers "does the kernel/driver accept this
        // pointer at all" independently of sparse compatibility.
        Memory m = ctx.ImportHost(s.ptr, len, ~0u, false);
        v += ", import " + (m.mem ? std::string("OK") : m.note);
        ctx.Free(m);
        v += ", sparse-compatible types " + TypeBits(hp.memoryTypeBits & sparse_bits);
        p.r.Fact(std::string("host[") + s.name + "]", v);
    }
}

static void DmaBufMatrix(Probe& p) {
    auto& ctx = p.ctx;
    p.r.Section("capabilities: udmabuf / dma-buf import");
    p.r.Fact("dmabuf.source", p.dbs.Available()
                                  ? (p.dbs.IsFake() ? "self-test memfd (fake)" : "/dev/udmabuf")
                                  : "<unavailable: " + p.dbs.Why() + ">");
    p.r.Fact("memfd.sealed(F_SEAL_SHRINK)", p.gm.Sealed() ? "yes" : "no: " + p.gm.SealError());
    if (!p.dbs.Available() || !ctx.ext_dmabuf) {
        return;
    }
    const uint32_t sparse_bits = p.arena_dmabuf.buf ? p.arena_dmabuf.reqs.memoryTypeBits : 0;
    auto describe = [&](const char* name, const DmaBuf& d) {
        if (!d) {
            p.r.Fact(name, "<" + d.error + ">");
            return;
        }
        VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        const VkResult pr =
            ctx.GetFdProps(ctx.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, d.fd, &fp);
        Memory m = ctx.ImportDmaBuf(d.fd, d.size, ~0u, false);
        p.r.Fact(name,
                 Sprintf("props %s bits %s, import %s, sparse-compatible types %s", ResultName(pr),
                         TypeBits(fp.memoryTypeBits).c_str(), m.mem ? "OK" : ResultName(m.result),
                         TypeBits(fp.memoryTypeBits & sparse_bits).c_str()));
        ctx.Free(m);
        close(d.fd);
    };
    const uint64_t pa = p.AllocPa(4 * p.unit, p.unit);
    describe("dmabuf[single 64K unit]", p.dbs.Single(pa, p.unit));
    if (p.dbs.SupportsList()) {
        std::vector<std::pair<uint64_t, uint64_t>> pieces;
        const uint64_t n = p.unit / p.piece;
        for (uint64_t k = 0; k < n; ++k) {
            // Reverse order, odd-piece offsets: neither contiguous nor block aligned.
            pieces.push_back({pa + (n - 1 - k) * 2 * p.piece + p.piece, p.piece});
        }
        describe("dmabuf[list of scattered pieces]", p.dbs.List(pieces));
    }
}

void RunCapabilities(Probe& p) {
    p.r.Section("device");
    p.ctx.DescribeDevice(p.r);
    p.r.Fact("probe.block(sparse granularity)", Hex(p.block));
    p.r.Fact("probe.unit(test region)", Hex(p.unit));
    p.r.Fact("probe.piece(guest granularity)", Hex(p.piece));
    p.r.Fact("arena[plain]", p.arena_plain.buf
                                 ? "align " + Hex(p.arena_plain.reqs.alignment) + " bits " +
                                       TypeBits(p.arena_plain.reqs.memoryTypeBits)
                                 : "<" + p.arena_plain_err + ">");
    p.r.Fact("arena[host]", p.arena_host.buf
                                ? "align " + Hex(p.arena_host.reqs.alignment) + " bits " +
                                      TypeBits(p.arena_host.reqs.memoryTypeBits)
                                : "<" + p.arena_host_err + ">");
    p.r.Fact("arena[dmabuf]", p.arena_dmabuf.buf
                                  ? "align " + Hex(p.arena_dmabuf.reqs.alignment) + " bits " +
                                        TypeBits(p.arena_dmabuf.reqs.memoryTypeBits)
                                  : "<" + p.arena_dmabuf_err + ">");
    p.r.Fact("export_pool(vk_export backend)",
             p.exp.Ok() ? p.exp.handle_name + ", memtype " + std::to_string(p.exp.mem.type) +
                              ", arena align " + Hex(p.exp.arena.reqs.alignment) + " bits " +
                              TypeBits(p.exp.arena.reqs.memoryTypeBits)
                        : "<" + p.exp.error + ">");
    if (!p.opt.Enabled("caps")) {
        return;
    }
    SparseMatrix(p);
    HostPointerMatrix(p);
    DmaBufMatrix(p);
}

} // namespace e0b
