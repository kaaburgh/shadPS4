// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Functional tests: data actually moves between the guest CPU view (memfd mappings) and the
// GPU view (sparse arena through BDA or storage descriptors).
//
//  T1  bulk import + sparse alias      one PA range bound at two arena offsets (VA aliases)
//  T1b plain (non-sparse) import       "plan B" if sparse cannot take imported memory
//  T2  stitched scattered / non-congruent 64 KiB blocks built from 16 KiB pieces
//  T3  overlapping aliases across two different VkDeviceMemory objects sharing pages
//  T4  CPU<->GPU coherence stress, optional DMA_BUF_IOCTL_SYNC variant
//  T5  remap identity: rebind the same arena block to another PA, ordered by the timeline
//  T6  submit/bind cost vs number of imported memory objects
//  T7  reads from a non-resident arena block

#include <algorithm>
#include <cstring>
#include <random>

#include "test_util.h"

namespace e0b {

namespace {

struct Segment {
    const uint8_t* ptr;
    uint64_t bytes;
};

/// A GPU round trip: CPU writes through a guest VA, GPU verifies, GPU writes, GPU re-reads
/// (possibly through an alias), then the CPU verifies through the given views.
struct RoundTrip {
    const Arena* arena;
    uint64_t gpu_read;   // arena offset the GPU reads the CPU-written data from
    uint64_t gpu_write;  // arena offset the GPU writes to
    uint64_t gpu_reread; // arena offset the GPU re-reads its own write from
    uint8_t* cpu_write;  // guest VA the CPU writes through
    uint64_t bytes;
    std::vector<std::pair<std::string, std::vector<Segment>>> cpu_views;
};

Checks DoRoundTrip(Probe& p, Path path, const RoundTrip& rt) {
    const uint32_t count = static_cast<uint32_t>(rt.bytes / 4);
    const uint32_t s1 = p.NextSeed();
    const uint32_t s2 = p.NextSeed();
    CpuWrite(rt.cpu_write, count, s1);

    GpuBatch b(p.ctx);
    const uint32_t i0 = b.Verify(path, ArenaView(*rt.arena, rt.gpu_read), count, s1);
    b.Barrier();
    b.Write(path, ArenaView(*rt.arena, rt.gpu_write), count, s2);
    b.Barrier();
    const uint32_t i1 = b.Verify(path, ArenaView(*rt.arena, rt.gpu_reread), count, s2);
    b.HostBarrier();
    b.Submit();

    Checks c;
    c.Gpu("cpu->gpu", b.Slot(i0), count);
    c.Gpu(rt.gpu_reread != rt.gpu_write ? "gpu->gpu(alias)" : "gpu->gpu", b.Slot(i1), count);
    for (const auto& [name, segs] : rt.cpu_views) {
        CpuCheck total;
        uint32_t index = 0;
        for (const auto& seg : segs) {
            const CpuCheck part = CpuVerify(seg.ptr, seg.bytes / 4, s2, index);
            if (part.mismatches && !total.mismatches) {
                total.first_bad = index + part.first_bad;
                total.first_value = part.first_value;
            }
            total.mismatches += part.mismatches;
            index += static_cast<uint32_t>(seg.bytes / 4);
        }
        c.Cpu(name, total);
    }
    return c;
}

const Arena& NeedArena(Probe& p, Backend b) {
    if (p.ctx.IsLavapipe() && b != Backend::DeviceLocal && b != Backend::VkHostVisible) {
        throw Unsupported(
            "lavapipe only sparse-binds its own allocations (imports/exports are ignored)");
    }
    std::string why;
    const Arena* a = p.ArenaFor(b, &why);
    if (!a) {
        throw Unsupported("no sparse arena for this handle type: " + why);
    }
    return *a;
}

void NeedBacking(const Backing& b, const Probe& p) {
    if (!b.Ok()) {
        if (b.unsupported) {
            throw Unsupported(b.error);
        }
        throw std::runtime_error(b.error);
    }
    if (b.mem_offset % p.block) {
        throw Unsupported(
            "memory offset " + Hex(b.mem_offset) +
            " is not sparse-block aligned (self-test fake dma-buf cannot express it)");
    }
}

void Bind(Probe& p, const Arena& a, const std::vector<BindOp>& ops) {
    p.ctx.BindSparse(a, ops, {}, {}, true);
}

void Unbind(Probe& p, const Arena& a, const std::vector<BindOp>& ops) noexcept {
    if (p.ctx.device_lost || ops.empty()) {
        return;
    }
    std::vector<BindOp> u;
    for (const auto& op : ops) {
        u.push_back({op.arena_offset, op.size, VK_NULL_HANDLE, 0});
    }
    try {
        p.ctx.BindSparse(a, u, {}, {}, true);
    } catch (...) {
    }
}

/// A physically contiguous guest region of `len` bytes provided by one of the bulk backends:
/// memfd imported through a host pointer or udmabuf, or exported Vulkan memory mmap'ed as
/// guest memory (inverted ownership).
struct BulkRegion {
    Probe& p;
    Backend be;
    uint64_t len;
    Backing bk; // imports only; the export pool is shared and not owned here
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint64_t mem_offset = 0;
    uint64_t pa = 0; // memfd offset or export pool offset
    uint8_t* canonical = nullptr;
    int sync_fd = -1;
    bool guest_mappable = true; // false: the CPU view is only the vkMapMemory pointer

    BulkRegion(Probe& p_, Backend be_, uint64_t len_, const Arena& arena, bool keep_fd = false)
        : p(p_), be(be_), len(len_) {
        if (be == Backend::VkHostVisible) {
            bk.mem = p.ctx.AllocateHostVisible(len, arena.reqs.memoryTypeBits);
            if (!bk.mem) {
                throw Unsupported(bk.mem.note);
            }
            mem = bk.mem.mem;
            canonical = static_cast<uint8_t*>(bk.mem.map);
            guest_mappable = false;
            return;
        }
        if (be == Backend::VkExport) {
            if (!p.exp.Ok()) {
                throw Unsupported("exported memory unavailable: " + p.exp.error);
            }
            pa = p.AllocExport(len);
            mem = p.exp.mem.mem;
            mem_offset = pa;
            canonical = p.exp.base + pa;
            return;
        }
        pa = p.AllocPa(len, p.unit);
        bk = be == Backend::HostBulk ? p.HostCanonical(pa, len, arena.reqs.memoryTypeBits)
                                     : p.DmaBufSingle(pa, len, arena.reqs.memoryTypeBits, keep_fd);
        try {
            NeedBacking(bk, p);
        } catch (...) {
            p.Release(bk);
            throw;
        }
        mem = bk.mem.mem;
        mem_offset = bk.mem_offset;
        canonical = p.gm.Canonical(pa);
        sync_fd = bk.dmabuf_fd;
    }
    ~BulkRegion() {
        p.Release(bk);
    }
    void MapGuest(uint64_t va, uint64_t off, uint64_t size) {
        if (!guest_mappable) {
            return;
        }
        if (be == Backend::VkExport) {
            p.gm.MapFd(va, p.exp.fd, pa + off, size);
        } else {
            p.gm.Map(va, pa + off, size);
        }
    }
    uint32_t MemType() const {
        return be == Backend::VkExport ? p.exp.mem.type : bk.mem.type;
    }
};

constexpr Backend kBulkBackends[] = {Backend::HostBulk, Backend::DmaBufBulk, Backend::VkExport,
                                     Backend::VkHostVisible};

/// Pointer the CPU writes through / reads the alias view from: a guest VA when the backend can
/// be mapped into the guest window, otherwise the vkMapMemory pointer.
uint8_t* CpuPtr(Probe& p, const BulkRegion& reg, uint64_t va, uint64_t off = 0) {
    return reg.guest_mappable ? p.gm.Gva(va) + off : reg.canonical + off;
}

// ---------------------------------------------------------------------------------------------

void T1BulkAlias(Probe& p) {
    for (const Backend be : kBulkBackends) {
        const std::string test = "T1.bulk_import+sparse_alias";
        RunVariant(p, test, BackendName(be), [&] {
            const Arena& arena = NeedArena(p, be);
            const uint64_t R = 4 * p.unit;
            BulkRegion reg(p, be, R, arena);
            const uint64_t va1 = p.AllocVa(R);
            const uint64_t va2 = p.AllocVa(R);
            reg.MapGuest(va1, 0, R);
            reg.MapGuest(va2, 0, R);
            Defer unmap([&] {
                p.gm.Unmap(va1, R);
                p.gm.Unmap(va2, R);
            });
            std::vector<BindOp> ops = {{va1, R, reg.mem, reg.mem_offset}};
            if (arena.aliased) {
                ops.push_back({va2, R, reg.mem, reg.mem_offset});
            }
            Bind(p, arena, ops);
            Defer unbind([&] { Unbind(p, arena, ops); });

            const uint64_t reread = arena.aliased ? va2 : va1;
            const std::string extra = Sprintf(
                "memtype %u%s", reg.MemType(),
                arena.aliased ? "" : ", no sparse alias (sparseResidencyAliased unsupported)");
            for (const Path path : Paths(p)) {
                RoundTrip rt{&arena, reread, va1, reread, CpuPtr(p, reg, va1), R, {}};
                if (reg.guest_mappable) {
                    rt.cpu_views.push_back({"gpu->cpu(alias va)", {{p.gm.Gva(va2), R}}});
                }
                rt.cpu_views.push_back(
                    {reg.guest_mappable ? "gpu->cpu(canonical)" : "gpu->cpu(vkMapMemory)",
                     {{reg.canonical, R}}});
                AddChecks(p, test, std::string(BackendName(be)) + "/" + PathName(path),
                          DoRoundTrip(p, path, rt), extra);
            }
        });
    }
}

void T1bPlainImport(Probe& p) {
    for (const Backend be : {Backend::HostBulk, Backend::DmaBufBulk}) {
        const std::string test = "T1b.plain_buffer_import(planB)";
        RunVariant(p, test, BackendName(be), [&] {
            auto& ctx = p.ctx;
            if (be == Backend::HostBulk && !ctx.ext_host) {
                throw Unsupported("VK_EXT_external_memory_host not available");
            }
            if (be == Backend::DmaBufBulk && !ctx.ext_dmabuf) {
                throw Unsupported("VK_EXT_external_memory_dma_buf not available");
            }
            const uint64_t R = 4 * p.unit;
            const uint64_t pa = p.AllocPa(R, p.unit);
            const uint64_t va1 = p.AllocVa(R);
            const uint64_t va2 = p.AllocVa(R);
            p.gm.Map(va1, pa, R);
            p.gm.Map(va2, pa, R);
            Defer unmap([&] {
                p.gm.Unmap(va1, R);
                p.gm.Unmap(va2, R);
            });

            VkExternalMemoryBufferCreateInfo ext{
                VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
            ext.handleTypes = be == Backend::HostBulk
                                  ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
                                  : VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.pNext = &ext;
            bci.size = R;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        (ctx.f_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
            Arena plain; // reuse the view helpers; not sparse
            E0B_CHECK(vkCreateBuffer(ctx.device, &bci, nullptr, &plain.buf));
            Defer destroy([&] { vkDestroyBuffer(ctx.device, plain.buf, nullptr); });
            vkGetBufferMemoryRequirements(ctx.device, plain.buf, &plain.reqs);
            plain.size = R;
            Backing bk = be == Backend::HostBulk
                             ? p.HostCanonical(pa, R, plain.reqs.memoryTypeBits, true)
                             : p.DmaBufSingle(pa, R, plain.reqs.memoryTypeBits, false, true);
            Defer release([&] { p.Release(bk); });
            if (!bk.Ok()) {
                NeedBacking(bk, p);
            }
            if (bk.mem_offset % plain.reqs.alignment) {
                throw Unsupported("memory offset not aligned for vkBindBufferMemory");
            }
            ctx.CheckResult(vkBindBufferMemory(ctx.device, plain.buf, bk.mem.mem, bk.mem_offset),
                            "vkBindBufferMemory(imported)");
            if (ctx.f_bda) {
                VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
                ai.buffer = plain.buf;
                plain.addr = vkGetBufferDeviceAddress(ctx.device, &ai);
            }
            for (const Path path : Paths(p)) {
                RoundTrip rt{&plain,
                             0,
                             0,
                             0,
                             p.gm.Gva(va1),
                             R,
                             {{"gpu->cpu(alias va)", {{p.gm.Gva(va2), R}}},
                              {"gpu->cpu(canonical)", {{p.gm.Canonical(pa), R}}}}};
                AddChecks(p, test, std::string(BackendName(be)) + "/" + PathName(path),
                          DoRoundTrip(p, path, rt), Sprintf("memtype %u", bk.mem.type));
            }
        });
    }
}

/// Where an imported memory object becomes visible to the GPU: bound into the sparse arena at
/// the guest VA (the shadPS4 design), or bound to its own plain buffer ("plan B": BDA page table
/// entries pointing at per-import buffers, used if sparse cannot take imported memory).
struct Placement {
    Probe& p;
    bool plan_b;
    const Arena* arena = nullptr; // view source
    uint64_t view_base = 0;       // arena offset of the placed range
    Arena plain;
    const Arena* sparse = nullptr;
    std::vector<BindOp> ops;

    Placement(Probe& p_, bool plan_b_) : p(p_), plan_b(plan_b_) {}
    ~Placement() {
        if (sparse) {
            Unbind(p, *sparse, ops);
        }
        if (plain.buf) {
            vkDestroyBuffer(p.ctx.device, plain.buf, nullptr);
        }
    }

    /// Memory types the import must use, and whether it needs the device-address flag.
    uint32_t Prepare(Backend be, uint64_t size) {
        if (!plan_b) {
            sparse = &NeedArena(p, be);
            arena = sparse;
            return sparse->reqs.memoryTypeBits;
        }
        auto& ctx = p.ctx;
        const bool host = be == Backend::HostBulk || be == Backend::HostGva;
        if (host ? !ctx.ext_host : !ctx.ext_dmabuf) {
            throw Unsupported(host ? "VK_EXT_external_memory_host not available"
                                   : "VK_EXT_external_memory_dma_buf not available");
        }
        VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext.handleTypes = host ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT
                               : VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.pNext = &ext;
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    (ctx.f_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        E0B_CHECK(vkCreateBuffer(ctx.device, &bci, nullptr, &plain.buf));
        vkGetBufferMemoryRequirements(ctx.device, plain.buf, &plain.reqs);
        plain.size = size;
        arena = &plain;
        return plain.reqs.memoryTypeBits;
    }

    /// Makes [bk.mem_offset, +size) visible at view_base (sparse: at `va`).
    void Place(const Backing& bk, uint64_t va, uint64_t size) {
        if (sparse) {
            ops.push_back({va, size, bk.mem.mem, bk.mem_offset});
            Bind(p, *sparse, {ops.back()});
            view_base = va;
            return;
        }
        if (bk.mem_offset % plain.reqs.alignment) {
            throw Unsupported("memory offset not aligned for vkBindBufferMemory");
        }
        p.ctx.CheckResult(vkBindBufferMemory(p.ctx.device, plain.buf, bk.mem.mem, bk.mem_offset),
                          "vkBindBufferMemory(imported)");
        if (p.ctx.f_bda) {
            VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            ai.buffer = plain.buf;
            plain.addr = vkGetBufferDeviceAddress(p.ctx.device, &ai);
        }
        view_base = 0;
    }
};

void T2Stitched(Probe& p) {
    const uint64_t A = p.unit;
    const uint64_t P = p.piece;
    const uint64_t n = A / P;

    for (const bool plan_b : {false, true}) {
        // T2a: one 64 KiB unit assembled from n scattered, non-congruent PA pieces.
        for (const Backend be : {Backend::HostGva, Backend::DmaBufList}) {
            const std::string test =
                plan_b ? "T2.stitched_scattered(planB)" : "T2.stitched_scattered";
            RunVariant(p, test, BackendName(be), [&] {
                Placement pl(p, plan_b);
                const uint32_t bits = pl.Prepare(be, A);
                const uint64_t region = p.AllocPa(2 * n * P, A);
                std::vector<std::pair<uint64_t, uint64_t>> pieces;
                const uint64_t va = p.AllocVa(A);
                for (uint64_t k = 0; k < n; ++k) {
                    const uint64_t q = region + (n - 1 - k) * 2 * P + P;
                    pieces.push_back({q, P});
                    p.gm.Map(va + k * P, q, P);
                }
                Defer unmap([&] { p.gm.Unmap(va, A); });
                Backing bk = be == Backend::HostGva ? p.HostGva(va, A, bits, plan_b)
                                                    : p.DmaBufList(pieces, bits, false, plan_b);
                Defer release([&] { p.Release(bk); });
                NeedBacking(bk, p);
                pl.Place(bk, va, A);
                std::vector<Segment> canon;
                for (const auto& [q, len] : pieces) {
                    canon.push_back({p.gm.Canonical(q), len});
                }
                for (const Path path : Paths(p)) {
                    const uint64_t v = pl.view_base;
                    RoundTrip rt{pl.arena,
                                 v,
                                 v,
                                 v,
                                 p.gm.Gva(va),
                                 A,
                                 {{"gpu->cpu(guest va)", {{p.gm.Gva(va), A}}},
                                  {"gpu->cpu(canonical pieces)", canon}}};
                    AddChecks(p, test, std::string(BackendName(be)) + "/" + PathName(path),
                              DoRoundTrip(p, path, rt),
                              Sprintf("%llu pieces of %s, bulk-importable: no",
                                      static_cast<unsigned long long>(n), Hex(P).c_str()));
                }
            });
        }
        if (plan_b) {
            continue;
        }
        // T2b: contiguous PA, misaligned to 64 KiB by one piece (VA != PA mod 64K).
        for (const Backend be :
             {Backend::HostBulk, Backend::HostGva, Backend::DmaBufBulk, Backend::DmaBufList}) {
            const std::string test = "T2.noncongruent_contiguous";
            RunVariant(p, test, BackendName(be), [&] {
                Placement pl(p, false);
                const uint32_t bits = pl.Prepare(be, A);
                const uint64_t r = p.AllocPa(A + P, A) + P;
                const uint64_t va = p.AllocVa(A);
                p.gm.Map(va, r, A);
                Defer unmap([&] { p.gm.Unmap(va, A); });
                Backing bk = be == Backend::HostBulk     ? p.HostCanonical(r, A, bits)
                             : be == Backend::HostGva    ? p.HostGva(va, A, bits)
                             : be == Backend::DmaBufBulk ? p.DmaBufSingle(r, A, bits, false)
                                                         : p.DmaBufList({{r, A}}, bits, false);
                Defer release([&] { p.Release(bk); });
                NeedBacking(bk, p);
                pl.Place(bk, va, A);
                for (const Path path : Paths(p)) {
                    RoundTrip rt{pl.arena,
                                 va,
                                 va,
                                 va,
                                 p.gm.Gva(va),
                                 A,
                                 {{"gpu->cpu(guest va)", {{p.gm.Gva(va), A}}},
                                  {"gpu->cpu(canonical)", {{p.gm.Canonical(r), A}}}}};
                    AddChecks(p, test, std::string(BackendName(be)) + "/" + PathName(path),
                              DoRoundTrip(p, path, rt),
                              "PA % 64K = " + Hex(r % A) + ", PA % sparse block = " +
                                  Hex(r % p.block) + " (import starts at the PA)");
                }
            });
        }
    }
}

void T3OverlapAlias(Probe& p) {
    const uint64_t A = p.unit;
    const uint64_t P = p.piece;
    const uint64_t n = A / P;
    const uint64_t h = n / 2;
    for (const bool plan_b : {false, true}) {
        for (const Backend be : {Backend::HostGva, Backend::DmaBufList}) {
            const std::string test =
                plan_b ? "T3.overlap_alias_barrier(planB)" : "T3.overlap_alias_barrier";
            RunVariant(p, test, BackendName(be), [&] {
                Placement pl1(p, plan_b);
                Placement pl2(p, plan_b);
                const uint32_t bits1 = pl1.Prepare(be, A);
                const uint32_t bits2 = pl2.Prepare(be, A);
                const uint64_t region = p.AllocPa((n + h) * 2 * P, A);
                auto u = [&](uint64_t i) { return region + i * 2 * P + P; };
                const uint64_t va5 = p.AllocVa(A);
                const uint64_t va6 = p.AllocVa(A);
                std::vector<std::pair<uint64_t, uint64_t>> l1, l2;
                for (uint64_t k = 0; k < n; ++k) {
                    l1.push_back({u(k), P});
                    l2.push_back({u(h + k), P});
                    p.gm.Map(va5 + k * P, u(k), P);
                    p.gm.Map(va6 + k * P, u(h + k), P);
                }
                Defer unmap([&] {
                    p.gm.Unmap(va5, A);
                    p.gm.Unmap(va6, A);
                });
                Backing b1 = be == Backend::HostGva ? p.HostGva(va5, A, bits1, plan_b)
                                                    : p.DmaBufList(l1, bits1, false, plan_b);
                Defer r1([&] { p.Release(b1); });
                NeedBacking(b1, p);
                Backing b2 = be == Backend::HostGva ? p.HostGva(va6, A, bits2, plan_b)
                                                    : p.DmaBufList(l2, bits2, false, plan_b);
                Defer r2([&] { p.Release(b2); });
                NeedBacking(b2, p);
                pl1.Place(b1, va5, A);
                pl2.Place(b2, va6, A);

                // Shared pages: object1 [h*P, n*P) == object2 [0, (n-h)*P).
                const uint32_t cnt = static_cast<uint32_t>((n - h) * P / 4);
                std::vector<Segment> canon;
                for (uint64_t k = h; k < n; ++k) {
                    canon.push_back({p.gm.Canonical(u(k)), P});
                }
                for (const Path path : Paths(p)) {
                    const uint32_t s = p.NextSeed();
                    GpuBatch b(p.ctx);
                    b.Write(path, ArenaView(*pl1.arena, pl1.view_base + h * P), cnt, s);
                    b.Barrier();
                    const uint32_t i = b.Verify(path, ArenaView(*pl2.arena, pl2.view_base), cnt, s);
                    b.HostBarrier();
                    b.Submit();
                    Checks c;
                    c.Gpu("gpu write via object1 -> gpu read via object2", b.Slot(i), cnt);
                    CpuCheck total;
                    uint32_t index = 0;
                    for (const auto& seg : canon) {
                        const CpuCheck part = CpuVerify(seg.ptr, seg.bytes / 4, s, index);
                        total.mismatches += part.mismatches;
                        index += static_cast<uint32_t>(seg.bytes / 4);
                    }
                    c.Cpu("gpu->cpu(canonical shared pages)", total);
                    c.Cpu("gpu->cpu(guest va of object2)", CpuVerify(p.gm.Gva(va6), cnt, s));
                    AddChecks(
                        p, test, std::string(BackendName(be)) + "/" + PathName(path), c,
                        Sprintf("%llu shared pieces", static_cast<unsigned long long>(n - h)));
                }

                // Same without a barrier: undefined by the spec, recorded for information only.
                const uint32_t s = p.NextSeed();
                GpuBatch b(p.ctx);
                const Path path = PreferredPath(p);
                b.Write(path, ArenaView(*pl1.arena, pl1.view_base + h * P), cnt, s);
                const uint32_t i = b.Verify(path, ArenaView(*pl2.arena, pl2.view_base), cnt, s);
                b.HostBarrier();
                b.Submit();
                const SlotResult sr = b.Slot(i);
                p.r.Add(plan_b ? "T3.overlap_alias_no_barrier(planB)"
                               : "T3.overlap_alias_no_barrier",
                        std::string(BackendName(be)) + "/" + PathName(path), Status::Info,
                        Sprintf("mismatches %u of %u (no barrier: undefined, informational)",
                                sr.mismatches, cnt));
            });
        }
    }
}

void T4Stress(Probe& p) {
    struct Variant {
        Backend be;
        bool sync;
    };
    for (const Variant v : {Variant{Backend::HostBulk, false}, Variant{Backend::DmaBufBulk, false},
                            Variant{Backend::DmaBufBulk, true}, Variant{Backend::VkExport, false},
                            Variant{Backend::VkHostVisible, false}}) {
        const std::string test = "T4.coherence_stress";
        const std::string name =
            std::string(BackendName(v.be)) + (v.sync ? "+DMA_BUF_IOCTL_SYNC" : "");
        RunVariant(p, test, name, [&] {
            if (v.sync && p.dbs.IsFake()) {
                throw Unsupported("DMA_BUF_IOCTL_SYNC needs a real udmabuf");
            }
            const Arena& arena = NeedArena(p, v.be);
            const uint64_t R = 16 * p.unit;
            BulkRegion reg(p, v.be, R, arena, v.sync);
            const uint64_t va1 = p.AllocVa(R);
            const uint64_t va2 = p.AllocVa(R);
            reg.MapGuest(va1, 0, R);
            reg.MapGuest(va2, 0, R);
            Defer unmap([&] {
                p.gm.Unmap(va1, R);
                p.gm.Unmap(va2, R);
            });
            std::vector<BindOp> ops = {{va1, R, reg.mem, reg.mem_offset}};
            if (arena.aliased) {
                ops.push_back({va2, R, reg.mem, reg.mem_offset});
            }
            Bind(p, arena, ops);
            Defer unbind([&] { Unbind(p, arena, ops); });

            const Path path = PreferredPath(p);
            const uint64_t align_dw = path == Path::Ssbo ? p.ctx.ssbo_align / 4 : 1;
            const uint64_t total_dw = R / 4;
            const uint64_t read_va = arena.aliased ? va2 : va1;
            std::mt19937 rng(p.opt.seed);
            uint64_t cpu_to_gpu = 0, gpu_to_cpu = 0, bad_iters = 0, dwords = 0, sync_errors = 0;
            int first_sync_errno = 0;
            const auto sync = [&](bool start, bool read, bool write) {
                const int e = DmaBufSync(reg.sync_fd, start, read, write);
                if (e != 0) {
                    ++sync_errors;
                    first_sync_errno = first_sync_errno ? first_sync_errno : e;
                }
            };
            const double t0 = NowUs();
            for (uint32_t it = 0; it < p.opt.t4_iters; ++it) {
                uint64_t off = rng() % (total_dw - 1);
                off -= off % align_dw;
                const uint64_t len = 1 + rng() % std::min<uint64_t>(16384, total_dw - off);
                const uint32_t k = p.NextSeed();
                const uint32_t k2 = k ^ 0xA5A5A5A5u;
                if (v.sync) {
                    sync(true, false, true);
                }
                CpuWrite(CpuPtr(p, reg, va1, off * 4), len, k);
                if (v.sync) {
                    sync(false, false, true);
                }
                GpuBatch b(p.ctx, 2);
                const uint32_t i = b.Verify(path, ArenaView(arena, read_va + off * 4),
                                            static_cast<uint32_t>(len), k);
                b.Barrier();
                b.Write(path, ArenaView(arena, va1 + off * 4), static_cast<uint32_t>(len), k2);
                b.HostBarrier();
                b.Submit();
                if (v.sync) {
                    sync(true, true, false);
                }
                const CpuCheck c = CpuVerify(CpuPtr(p, reg, va2, off * 4), len, k2);
                if (v.sync) {
                    sync(false, true, false);
                }
                const SlotResult s = b.Slot(i);
                const uint64_t g = s.mismatches + (s.checked != len ? 1 : 0);
                cpu_to_gpu += g;
                gpu_to_cpu += c.mismatches;
                bad_iters += (g || c.mismatches) ? 1 : 0;
                dwords += len;
            }
            const double us = NowUs() - t0;
            const bool ok = cpu_to_gpu == 0 && gpu_to_cpu == 0;
            std::string detail =
                Sprintf("cpu->gpu mismatches %llu, gpu->cpu mismatches %llu, bad iterations "
                        "%llu of %u (%s)",
                        static_cast<unsigned long long>(cpu_to_gpu),
                        static_cast<unsigned long long>(gpu_to_cpu),
                        static_cast<unsigned long long>(bad_iters), p.opt.t4_iters, PathName(path));
            // The coherence verdict comes from the variants without the ioctl. DMA_BUF_IOCTL_SYNC
            // on a udmabuf only syncs udmabuf's own device mapping, so this variant is never
            // evidence of coherence: INFO when the ioctl works, UNSUPPORTED when it fails.
            Status status = ok ? Status::Pass : Status::Fail;
            if (v.sync) {
                if (sync_errors) {
                    status = Status::Unsupported;
                    detail = Sprintf("DMA_BUF_IOCTL_SYNC failed %llu times (first: %s); ",
                                     static_cast<unsigned long long>(sync_errors),
                                     std::strerror(first_sync_errno)) +
                             detail;
                } else {
                    status = Status::Info;
                    detail = "informational, not a coherence verdict: " + detail;
                }
            }
            p.r.Add(test, name, status, detail,
                    {{"iterations", double(p.opt.t4_iters)},
                     {"dwords", double(dwords)},
                     {"us_per_iteration", us / std::max<uint32_t>(1, p.opt.t4_iters)},
                     {"sync_ioctl_errors", double(sync_errors)}});
        });
    }
}

void T5Remap(Probe& p) {
    for (const Backend be : kBulkBackends) {
        const std::string test = "T5.remap_identity_timeline";
        RunVariant(p, test, BackendName(be), [&] {
            auto& ctx = p.ctx;
            const Arena& arena = NeedArena(p, be);
            const uint64_t A = p.unit;
            BulkRegion reg(p, be, 2 * A, arena);
            const uint64_t x = p.AllocVa(A);
            std::memset(reg.canonical, 0, 2 * A);
            Bind(p, arena, {{x, A, reg.mem, reg.mem_offset}});
            Defer unbind([&] { Unbind(p, arena, {{x, A, reg.mem, 0}}); });

            const Path path = PreferredPath(p);
            const uint32_t cnt = static_cast<uint32_t>(A / 4);
            const uint32_t sa = p.NextSeed();
            const uint32_t sb = p.NextSeed();
            const uint64_t v1 = ctx.NextTimelineValue();
            const uint64_t v2 = ctx.NextTimelineValue();
            const uint64_t v3 = ctx.NextTimelineValue();
            const uint64_t v4 = ctx.NextTimelineValue();

            GpuBatch b1(ctx, 1);
            b1.Write(path, ArenaView(arena, x), cnt, sa);
            b1.HostBarrier();
            b1.Submit({}, v1, false);
            // Rebind the same arena block to the next PA only after b1 has finished with it.
            const double rebind_us =
                ctx.BindSparse(arena, {{x, A, reg.mem, reg.mem_offset + A}}, v1, v2, false);
            GpuBatch b2(ctx, 1);
            b2.Write(path, ArenaView(arena, x), cnt, sb);
            b2.HostBarrier();
            b2.Submit(v2, v3, false);
            const double unbind_us =
                ctx.BindSparse(arena, {{x, A, VK_NULL_HANDLE, 0}}, v3, v4, false);
            const double t0 = NowUs();
            ctx.WaitTimeline(v4);
            const double wait_us = NowUs() - t0;
            b1.WaitHost();
            b2.WaitHost();

            Checks c;
            c.Cpu("PA0 holds write before rebind", CpuVerify(reg.canonical, cnt, sa));
            c.Cpu("PA1 holds write after rebind", CpuVerify(reg.canonical + A, cnt, sb));
            p.r.Add(test, std::string(BackendName(be)) + "/" + PathName(path),
                    c.Clean() ? Status::Pass : Status::Fail, c.Detail(),
                    {{"rebind_call_us", rebind_us},
                     {"unbind_call_us", unbind_us},
                     {"chain_wait_us", wait_us}});
        });
    }
}

void T6Scaling(Probe& p) {
    const std::vector<uint32_t> scale = ScaleSteps(ObjectCap(p, "T6.max_objects"));
    if (scale.empty()) {
        return;
    }
    const uint32_t max_n = scale.back();
    uint64_t region_va = 0;
    uint64_t region_pa = 0;
    try {
        region_va = p.AllocVa(uint64_t(max_n) * p.block);
        region_pa = p.AllocPa(uint64_t(max_n) * p.block, p.block);
    } catch (const std::exception& e) {
        p.r.Add("T6.bo_scaling", "all", Status::Skip, e.what());
        return;
    }
    for (const Backend be : {Backend::DeviceLocal, Backend::HostBulk, Backend::DmaBufBulk}) {
        std::string limit_hit; // set once object creation runs out of a resource
        for (const uint32_t n : scale) {
            const std::string variant = Sprintf("%s/N=%u", BackendName(be), n);
            if (!limit_hit.empty()) {
                p.r.Add("T6.bo_scaling", variant, Status::Skip,
                        "resource limit reached at a smaller N: " + limit_hit);
                continue;
            }
            RunVariant(p, "T6.bo_scaling", variant, [&] {
                auto& ctx = p.ctx;
                const Arena& arena = NeedArena(p, be);
                std::vector<Backing> objs;
                objs.reserve(n);
                Defer release([&] {
                    for (auto& o : objs) {
                        p.Release(o);
                    }
                });
                const double t0 = NowUs();
                for (uint32_t i = 0; i < n; ++i) {
                    const uint64_t pa = region_pa + uint64_t(i) * p.block;
                    Backing b;
                    if (be == Backend::DeviceLocal) {
                        b.mem = ctx.AllocateDevice(p.block, arena.reqs.memoryTypeBits);
                        if (!b.mem) {
                            b.error = b.mem.note;
                        }
                    } else if (be == Backend::HostBulk) {
                        b = p.HostCanonical(pa, p.block, arena.reqs.memoryTypeBits);
                    } else {
                        b = p.DmaBufSingle(pa, p.block, arena.reqs.memoryTypeBits, false);
                    }
                    if (!b.Ok() || b.mem_offset % p.block) {
                        const bool unsup = b.unsupported || (b.Ok() && b.mem_offset % p.block);
                        const std::string why = b.Ok() ? "offset not block aligned" : b.error;
                        p.Release(b);
                        if (i == 0) {
                            if (unsup) {
                                throw Unsupported(why);
                            }
                            throw std::runtime_error(why);
                        }
                        if (ctx.device_lost) {
                            throw std::runtime_error(
                                Sprintf("object %u of %u: %s", i, n, why.c_str()));
                        }
                        // The first objects were created fine: running out now is a limit
                        // (allocation count, pinned memory, fds), not a feasibility error.
                        limit_hit = Sprintf("object %u of %u: %s", i, n, why.c_str());
                        p.r.Add("T6.bo_scaling", variant, Status::Info,
                                "stopped, resource limit reached at " + limit_hit,
                                {{"objects_created", double(i)}});
                        return;
                    }
                    objs.push_back(b);
                }
                const double create_us = NowUs() - t0;
                std::vector<BindOp> ops;
                for (uint32_t i = 0; i < n; ++i) {
                    ops.push_back({region_va + uint64_t(i) * p.block, p.block, objs[i].mem.mem,
                                   objs[i].mem_offset});
                }
                double bind_wall = 0;
                const double bind_call = ctx.BindSparse(arena, ops, {}, {}, true, &bind_wall);
                Defer unbind([&] { Unbind(p, arena, ops); });

                const Path path = PreferredPath(p);
                GpuBatch empty(ctx, 1);
                GpuBatch tiny(ctx, 1);
                tiny.Write(path, ArenaView(arena, region_va), 1, p.NextSeed());
                std::vector<double> te, tt;
                for (uint32_t k = 0; k < p.opt.t6_iters; ++k) {
                    double s = NowUs();
                    empty.Resubmit();
                    te.push_back(NowUs() - s);
                    s = NowUs();
                    tiny.Resubmit();
                    tt.push_back(NowUs() - s);
                }
                std::vector<BindOp> un;
                for (const auto& op : ops) {
                    un.push_back({op.arena_offset, op.size, VK_NULL_HANDLE, 0});
                }
                double unbind_wall = 0;
                ctx.BindSparse(arena, un, {}, {}, true, &unbind_wall);
                ops.clear();
                const double f0 = NowUs();
                for (auto& o : objs) {
                    p.Release(o);
                }
                objs.clear();
                const double free_us = NowUs() - f0;
                p.r.Add("T6.bo_scaling", variant, Status::Info, "",
                        {{"create_total_us", create_us},
                         {"bind_call_us", bind_call},
                         {"bind_wall_us", bind_wall},
                         {"empty_submit_wait_median_us", Median(te)},
                         {"tiny_dispatch_submit_wait_median_us", Median(tt)},
                         {"unbind_wall_us", unbind_wall},
                         {"free_total_us", free_us}});
            });
        }
    }
}

void T7NonResident(Probe& p) {
    for (const Path path : Paths(p)) {
        RunVariant(p, "T7.nonresident_read", PathName(path), [&] {
            const Arena& arena = NeedArena(p, Backend::DeviceLocal);
            const uint64_t z = p.AllocVa(p.block);
            const uint32_t cnt = static_cast<uint32_t>(p.block / 4);
            GpuBatch b(p.ctx, 1);
            const uint32_t i = b.NonZero(path, ArenaView(arena, z), cnt);
            b.HostBarrier();
            b.Submit();
            const SlotResult s = b.Slot(i);
            p.r.Add(
                "T7.nonresident_read", PathName(path), Status::Info,
                Sprintf("non-zero dwords %u of %u (checked %u); residencyNonResidentStrict=%s",
                        s.mismatches, cnt, s.checked,
                        p.ctx.props.sparseProperties.residencyNonResidentStrict ? "yes" : "no"));
        });
    }
}

} // namespace

void RunFunctional(Probe& p) {
    p.r.Section("functional tests");
    if (!p.arena_plain.buf && !p.arena_host.buf && !p.arena_dmabuf.buf) {
        p.r.Note("no sparse arena could be created; only plan B (T1b) can run");
    }
    if (p.opt.Enabled("t1")) {
        T1BulkAlias(p);
        T1bPlainImport(p);
    }
    if (p.opt.Enabled("t2")) {
        T2Stitched(p);
    }
    if (p.opt.Enabled("t3")) {
        T3OverlapAlias(p);
    }
    if (p.opt.Enabled("t5")) {
        T5Remap(p);
    }
    if (p.opt.Enabled("t4")) {
        T4Stress(p);
    }
    if (p.opt.Enabled("t7")) {
        T7NonResident(p);
    }
    if (p.opt.Enabled("t6")) {
        T6Scaling(p);
    }
}

} // namespace e0b
