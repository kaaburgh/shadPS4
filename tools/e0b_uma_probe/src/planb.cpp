// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Plan-B tests: ordinary (non-sparse) VkBuffers bound to imported guest memory, the path that
// remains when a sparse arena cannot take imported memory (NVIDIA: memoryTypeBits mismatch).
//
//  T4b  CPU<->GPU coherence stress through an imported buffer, BDA and SSBO
//  T5b  remap identity: guest VA moves from backing A to B; old import destroyed, new created
//  T6b  import/submit/destroy cost vs number of imported buffers
//
// The tests are written against Importer only. An Importer turns "whatever physical memory is
// mapped at guest VA [va, va+len) right now" into one VkDeviceMemory; HostImporter does it
// with VK_EXT_external_memory_host on the guest VA pointer, DmaBufImporter with udmabuf over
// the memfd pieces mapped there. A further mechanism only needs another Importer.

#include <cstring>
#include <memory>
#include <random>

#include "test_util.h"

namespace e0b {

namespace {

/// One ordinary VkBuffer bound to one imported memory object.
struct PlainImport {
    uint64_t id = 0;
    Arena view; // buffer, device address, size, requirements (not sparse)
    Backing bk;
};

class Importer {
public:
    explicit Importer(Probe& p) : p_(p) {}
    virtual ~Importer() = default;

    virtual const char* Name() const = 0;
    virtual VkExternalMemoryHandleTypeFlagBits Handle() const = 0;
    /// Empty when the mechanism can be used on this device/system.
    virtual std::string Unavailable() const = 0;
    /// Human-readable source of an import of [va, va+len), for lifecycle logs.
    virtual std::string Describe(uint64_t va, uint64_t len) const = 0;

    /// Creates an ordinary buffer over whatever is mapped at guest [va, va+len) now.
    PlainImport Create(uint64_t va, uint64_t len) {
        const std::string why = Unavailable();
        if (!why.empty()) {
            throw Unsupported(why);
        }
        auto& ctx = p_.ctx;
        PlainImport imp;
        imp.id = next_id_++;
        VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext.handleTypes = Handle();
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.pNext = &ext;
        bci.size = len;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                    VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    (ctx.f_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
        ctx.CheckResult(vkCreateBuffer(ctx.device, &bci, nullptr, &imp.view.buf),
                        "vkCreateBuffer(import)");
        vkGetBufferMemoryRequirements(ctx.device, imp.view.buf, &imp.view.reqs);
        imp.view.size = len;
        imp.bk = ImportVa(va, len, imp.view.reqs.memoryTypeBits);
        if (!imp.bk.Ok()) {
            const Backing failed = imp.bk;
            Destroy(imp);
            if (failed.unsupported) {
                throw Unsupported(failed.error);
            }
            throw std::runtime_error(failed.error);
        }
        if (imp.bk.mem_offset % imp.view.reqs.alignment) {
            Destroy(imp);
            throw Unsupported("import offset not aligned for vkBindBufferMemory");
        }
        const VkResult r =
            vkBindBufferMemory(ctx.device, imp.view.buf, imp.bk.mem.mem, imp.bk.mem_offset);
        if (r != VK_SUCCESS) {
            Destroy(imp);
            ctx.CheckResult(r, "vkBindBufferMemory(import)");
        }
        if (ctx.f_bda) {
            VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
            ai.buffer = imp.view.buf;
            imp.view.addr = vkGetBufferDeviceAddress(ctx.device, &ai);
        }
        return imp;
    }

    /// Destroys buffer and memory. The caller must have waited for all GPU work using it.
    void Destroy(PlainImport& imp) {
        if (imp.view.buf) {
            vkDestroyBuffer(p_.ctx.device, imp.view.buf, nullptr);
            imp.view.buf = VK_NULL_HANDLE;
        }
        p_.Release(imp.bk);
    }

protected:
    /// Mechanism-specific import; bda_flag is always needed for plain BDA buffers.
    virtual Backing ImportVa(uint64_t va, uint64_t len, uint32_t bits) = 0;

    Probe& p_;

private:
    uint64_t next_id_ = 1;
};

/// VK_EXT_external_memory_host on the guest VA pointer: pins exactly the memfd pages the
/// guest has mapped there, however they are stitched.
class HostImporter final : public Importer {
public:
    using Importer::Importer;
    const char* Name() const override {
        return "host";
    }
    VkExternalMemoryHandleTypeFlagBits Handle() const override {
        return VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    }
    std::string Unavailable() const override {
        return p_.ctx.ext_host ? std::string() : "VK_EXT_external_memory_host not available";
    }
    std::string Describe(uint64_t va, uint64_t len) const override {
        return Sprintf("host pointer %s, %s bytes",
                       Hex(reinterpret_cast<uintptr_t>(p_.gm.Gva(va))).c_str(), Hex(len).c_str());
    }

protected:
    Backing ImportVa(uint64_t va, uint64_t len, uint32_t bits) override {
        return p_.HostGva(va, len, bits, true);
    }
};

/// udmabuf over the memfd pieces currently mapped at the guest VA, imported with
/// VK_EXT_external_memory_dma_buf (the AMD path).
class DmaBufImporter final : public Importer {
public:
    using Importer::Importer;
    const char* Name() const override {
        return "dmabuf";
    }
    VkExternalMemoryHandleTypeFlagBits Handle() const override {
        return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    }
    std::string Unavailable() const override {
        if (!p_.ctx.ext_dmabuf) {
            return "VK_EXT_external_memory_dma_buf not available";
        }
        if (!p_.dbs.Available()) {
            return p_.dbs.Why();
        }
        return {};
    }
    std::string Describe(uint64_t va, uint64_t len) const override {
        std::vector<std::pair<uint64_t, uint64_t>> pieces;
        std::string why;
        if (!p_.gm.Pieces(va, len, &pieces, &why)) {
            return why;
        }
        return Sprintf("%s over %zu memfd piece(s), first PA %s",
                       p_.dbs.IsFake() ? "self-test memfd" : "udmabuf", pieces.size(),
                       Hex(pieces[0].first).c_str());
    }

protected:
    Backing ImportVa(uint64_t va, uint64_t len, uint32_t bits) override {
        std::vector<std::pair<uint64_t, uint64_t>> pieces;
        std::string why;
        if (!p_.gm.Pieces(va, len, &pieces, &why)) {
            Backing b;
            b.error = why;
            return b;
        }
        if (pieces.size() == 1) {
            return p_.DmaBufSingle(pieces[0].first, pieces[0].second, bits, false, true);
        }
        return p_.DmaBufList(pieces, bits, false, true);
    }
};

std::vector<std::unique_ptr<Importer>> MakeImporters(Probe& p) {
    std::vector<std::unique_ptr<Importer>> v;
    v.push_back(std::make_unique<HostImporter>(p));
    v.push_back(std::make_unique<DmaBufImporter>(p));
    return v;
}

/// Destroys a PlainImport on scope exit unless it was destroyed already.
class ImportGuard {
public:
    ImportGuard(Importer& im, PlainImport& imp) : im_(im), imp_(imp) {}
    ~ImportGuard() {
        im_.Destroy(imp_);
    }

private:
    Importer& im_;
    PlainImport& imp_;
};

/// Ordered, timestamped lifecycle log: printed as it happens and kept for the JSON detail.
class Lifecycle {
public:
    Lifecycle(Probe& p, std::string tag) : p_(p), tag_(std::move(tag)), t0_(NowUs()) {}
    void Step(const std::string& what) {
        ++n_;
        p_.r.Note(
            Sprintf("%s step %u (+%.0f us): %s", tag_.c_str(), n_, NowUs() - t0_, what.c_str()));
        log_ += Sprintf("%s%u) %s", log_.empty() ? "" : " | ", n_, what.c_str());
    }
    const std::string& Log() const {
        return log_;
    }

private:
    Probe& p_;
    std::string tag_;
    double t0_;
    uint32_t n_ = 0;
    std::string log_;
};

// ---------------------------------------------------------------------------------------------

void T4bStress(Probe& p, Importer& im) {
    const std::string test = "T4b.planb_coherence_stress";
    for (const Path path : Paths(p)) {
        RunVariant(p, test, std::string(im.Name()) + "/" + PathName(path), [&] {
            const uint64_t R = 16 * p.unit;
            const uint64_t pa = p.AllocPa(R, p.unit);
            const uint64_t va1 = p.AllocVa(R);
            const uint64_t va2 = p.AllocVa(R);
            p.gm.Map(va1, pa, R);
            p.gm.Map(va2, pa, R);
            Defer unmap([&] {
                p.gm.Unmap(va1, R);
                p.gm.Unmap(va2, R);
            });
            PlainImport imp = im.Create(va1, R);
            ImportGuard guard(im, imp);

            const uint64_t align_dw = path == Path::Ssbo ? p.ctx.ssbo_align / 4 : 1;
            const uint64_t total_dw = R / 4;
            std::mt19937 rng(p.opt.seed ^ static_cast<uint32_t>(path));
            uint64_t cpu_to_gpu = 0, unchecked = 0, gpu_to_alias = 0, gpu_to_canon = 0;
            uint64_t bad_iters = 0, dwords = 0;
            const double t0 = NowUs();
            for (uint32_t it = 0; it < p.opt.t4_iters; ++it) {
                uint64_t off = rng() % (total_dw - 1);
                off -= off % align_dw;
                const uint64_t len = 1 + rng() % std::min<uint64_t>(16384, total_dw - off);
                const uint32_t k = p.NextSeed();
                const uint32_t k2 = k ^ 0x5A5A5A5Au;
                // CPU writes through the guest VA the import was made from.
                CpuWrite(p.gm.Gva(va1) + off * 4, len, k);
                GpuBatch b(p.ctx, 2);
                const uint32_t i =
                    b.Verify(path, ArenaView(imp.view, off * 4), static_cast<uint32_t>(len), k);
                b.Barrier();
                b.Write(path, ArenaView(imp.view, off * 4), static_cast<uint32_t>(len), k2);
                b.HostBarrier();
                b.Submit();
                const SlotResult s = b.Slot(i);
                const uint64_t miss = s.checked < len ? len - s.checked : 0;
                const CpuCheck ca = CpuVerify(p.gm.Gva(va2) + off * 4, len, k2);
                const CpuCheck cc = CpuVerify(p.gm.Canonical(pa) + off * 4, len, k2);
                cpu_to_gpu += s.mismatches;
                unchecked += miss + (s.checked > len ? 1 : 0);
                gpu_to_alias += ca.mismatches;
                gpu_to_canon += cc.mismatches;
                bad_iters += (s.mismatches || miss || ca.mismatches || cc.mismatches) ? 1 : 0;
                dwords += len;
            }
            const double us = NowUs() - t0;
            Checks c;
            c.Count("cpu->gpu mismatches", cpu_to_gpu);
            c.Count("gpu dwords not checked", unchecked);
            c.Count("gpu->cpu(alias va) mismatches", gpu_to_alias);
            c.Count("gpu->cpu(canonical) mismatches", gpu_to_canon);
            p.r.Add(test, std::string(im.Name()) + "/" + PathName(path),
                    c.Clean() ? Status::Pass : Status::Fail,
                    c.Detail() +
                        Sprintf(" | bad iterations %llu of %u; GPU accesses the imported buffer "
                                "directly, no staging copies; memtype %u",
                                static_cast<unsigned long long>(bad_iters), p.opt.t4_iters,
                                imp.bk.mem.type),
                    {{"iterations", double(p.opt.t4_iters)},
                     {"dwords", double(dwords)},
                     {"us_per_iteration", us / std::max<uint32_t>(1, p.opt.t4_iters)}});
        });
    }
}

void T5bRemap(Probe& p, Importer& im) {
    const std::string test = "T5b.planb_remap_identity";
    for (const Path path : Paths(p)) {
        const std::string variant = std::string(im.Name()) + "/" + PathName(path);
        RunVariant(p, test, variant, [&] {
            if (const std::string why = im.Unavailable(); !why.empty()) {
                throw Unsupported(why);
            }
            auto& ctx = p.ctx;
            const uint64_t L = 4 * p.unit;
            const uint32_t cnt = static_cast<uint32_t>(L / 4);
            const uint64_t pa_a = p.AllocPa(L, p.unit);
            const uint64_t pa_b = p.AllocPa(L, p.unit);
            const uint64_t va = p.AllocVa(L);
            const uint32_t sa = p.NextSeed(), sb = p.NextSeed();
            const uint32_t wa = p.NextSeed(), wb = p.NextSeed();
            CpuWrite(p.gm.Canonical(pa_a), cnt, sa);
            CpuWrite(p.gm.Canonical(pa_b), cnt, sb);
            Lifecycle life(p, "T5b " + variant);
            Checks c;

            p.gm.Map(va, pa_a, L);
            Defer unmap([&] { p.gm.Unmap(va, L); });
            life.Step(Sprintf("map guest va %s -> backing A (PA %s)", Hex(va).c_str(),
                              Hex(pa_a).c_str()));

            // Phase 1: import what is mapped now (A), GPU reads A and writes into it.
            PlainImport imp1 = im.Create(va, L);
            ImportGuard g1(im, imp1);
            life.Step(Sprintf("import#%llu created over the va: %s, memtype %u",
                              static_cast<unsigned long long>(imp1.id), im.Describe(va, L).c_str(),
                              imp1.bk.mem.type));
            const uint64_t v1 = ctx.NextTimelineValue();
            GpuBatch b1(ctx, 2);
            const uint32_t s_a = b1.Verify(path, ArenaView(imp1.view, 0), cnt, sa);
            b1.Barrier();
            b1.Write(path, ArenaView(imp1.view, 0), cnt, wa);
            b1.HostBarrier();
            b1.Submit({}, v1, false);
            life.Step(Sprintf("batch 1 submitted through import#%llu (verify A, write wa), "
                              "signals timeline %llu",
                              static_cast<unsigned long long>(imp1.id),
                              static_cast<unsigned long long>(v1)));
            ctx.WaitTimeline(v1);
            b1.WaitHost();
            life.Step(Sprintf("host waited timeline %llu and the batch fence: no outstanding "
                              "GPU work references import#%llu",
                              static_cast<unsigned long long>(v1),
                              static_cast<unsigned long long>(imp1.id)));
            c.Gpu("phase1: gpu sees A", b1.Slot(s_a), cnt);
            c.Cpu("phase1: gpu write landed in A", CpuVerify(p.gm.Canonical(pa_a), cnt, wa));

            // Retire the old import before the guest mapping changes under it.
            const uint64_t id1 = imp1.id;
            im.Destroy(imp1);
            life.Step(Sprintf("import#%llu destroyed (vkDestroyBuffer + vkFreeMemory)",
                              static_cast<unsigned long long>(id1)));
            p.gm.Map(va, pa_b, L);
            life.Step(Sprintf("remap same guest va %s -> backing B (PA %s)", Hex(va).c_str(),
                              Hex(pa_b).c_str()));

            // Phase 2: a new import of the same va must reach B, never the stale A.
            PlainImport imp2 = im.Create(va, L);
            ImportGuard g2(im, imp2);
            life.Step(Sprintf("import#%llu created over the same va: %s, memtype %u",
                              static_cast<unsigned long long>(imp2.id), im.Describe(va, L).c_str(),
                              imp2.bk.mem.type));
            const uint64_t v2 = ctx.NextTimelineValue();
            GpuBatch b2(ctx, 3);
            const uint32_t s_b = b2.Verify(path, ArenaView(imp2.view, 0), cnt, sb);
            const uint32_t s_stale = b2.Verify(path, ArenaView(imp2.view, 0), cnt, wa);
            b2.Barrier();
            b2.Write(path, ArenaView(imp2.view, 0), cnt, wb);
            b2.HostBarrier();
            b2.Submit({}, v2, false);
            life.Step(Sprintf("batch 2 submitted through import#%llu (verify B, look for stale "
                              "A, write wb), signals timeline %llu",
                              static_cast<unsigned long long>(imp2.id),
                              static_cast<unsigned long long>(v2)));
            ctx.WaitTimeline(v2);
            b2.WaitHost();
            life.Step(Sprintf("host waited timeline %llu", static_cast<unsigned long long>(v2)));

            const SlotResult st = b2.Slot(s_stale);
            c.Gpu("phase2: gpu sees B", b2.Slot(s_b), cnt);
            c.Count("phase2: stale A dwords visible through new import",
                    st.checked >= st.mismatches ? st.checked - st.mismatches : 0,
                    st.checked == cnt ? "" : Sprintf("checked %u of %u", st.checked, cnt));
            if (st.checked != cnt) {
                c.Count("phase2: stale check incomplete", 1);
            }
            c.Cpu("phase2: gpu write landed in B", CpuVerify(p.gm.Canonical(pa_b), cnt, wb));
            c.Cpu("phase2: A untouched after remap", CpuVerify(p.gm.Canonical(pa_a), cnt, wa));

            const uint64_t id2 = imp2.id;
            im.Destroy(imp2);
            life.Step(Sprintf("import#%llu destroyed", static_cast<unsigned long long>(id2)));
            AddChecks(p, test, variant, c, "lifecycle: " + life.Log());
        });
    }
}

void T6bScaling(Probe& p, const std::vector<std::unique_ptr<Importer>>& importers) {
    const std::string test = "T6b.planb_import_scaling";
    const std::vector<uint32_t> scale = ScaleSteps(ObjectCap(p, "T6b.max_objects"));
    if (scale.empty()) {
        return;
    }
    const uint64_t obj = p.unit; // one 64 KiB guest range per imported buffer
    const uint32_t max_n = scale.back();
    uint64_t region_va = 0;
    try {
        region_va = p.AllocVa(uint64_t(max_n) * obj);
        const uint64_t region_pa = p.AllocPa(uint64_t(max_n) * obj, p.unit);
        p.gm.Map(region_va, region_pa, uint64_t(max_n) * obj);
    } catch (const std::exception& e) {
        p.r.Add(test, "all", Status::Skip, e.what());
        return;
    }
    Defer unmap([&] { p.gm.Unmap(region_va, uint64_t(max_n) * obj); });

    for (const auto& im : importers) {
        std::string limit_hit;
        for (const uint32_t n : scale) {
            const std::string variant = Sprintf("%s/N=%u", im->Name(), n);
            if (!limit_hit.empty()) {
                p.r.Add(test, variant, Status::Skip,
                        "resource limit reached at a smaller N: " + limit_hit);
                continue;
            }
            RunVariant(p, test, variant, [&] {
                auto& ctx = p.ctx;
                std::vector<PlainImport> objs;
                objs.reserve(n);
                Defer destroy_all([&] {
                    for (auto& o : objs) {
                        im->Destroy(o);
                    }
                });

                // 1. Initial import cost: buffer + memory import + bind, per object.
                const double t0 = NowUs();
                for (uint32_t i = 0; i < n; ++i) {
                    try {
                        objs.push_back(im->Create(region_va + uint64_t(i) * obj, obj));
                    } catch (const std::exception& e) {
                        if (i == 0 || ctx.device_lost) {
                            throw;
                        }
                        // The first objects worked: running out now is a limit (allocation
                        // count, pinned memory, fds), not a feasibility error.
                        limit_hit = Sprintf("object %u of %u: %s", i, n, e.what());
                        p.r.Add(test, variant, Status::Info,
                                "stopped, resource limit reached at " + limit_hit,
                                {{"objects_created", double(i)}});
                        return;
                    }
                }
                const double create_us = NowUs() - t0;

                const Path path = PreferredPath(p);
                const uint32_t seed = p.NextSeed();
                // 2. First submit after the imports, kept apart from the steady state: it may
                //    carry one-time residency work for the new objects.
                GpuBatch tiny(ctx, 1);
                tiny.Write(path, ArenaView(objs[0].view, 0), 1, seed);
                double t = NowUs();
                tiny.Submit();
                const double first_submit_us = NowUs() - t;

                // 3. Steady state: same command buffers resubmitted, after a short warm-up.
                GpuBatch empty(ctx, 1);
                for (int w = 0; w < 2; ++w) {
                    empty.Resubmit();
                    tiny.Resubmit();
                }
                std::vector<double> te, tt;
                for (uint32_t k = 0; k < p.opt.t6_iters; ++k) {
                    t = NowUs();
                    empty.Resubmit();
                    te.push_back(NowUs() - t);
                    t = NowUs();
                    tiny.Resubmit();
                    tt.push_back(NowUs() - t);
                }

                // 4. Working set: one tiny dispatch per imported buffer in one submit.
                const uint32_t seed_all = p.NextSeed();
                GpuBatch all(ctx, n);
                for (uint32_t i = 0; i < n; ++i) {
                    all.Write(path, ArenaView(objs[i].view, 0), 1, seed_all);
                }
                all.HostBarrier();
                t = NowUs();
                all.Submit();
                const double touch_first_us = NowUs() - t;
                std::vector<double> ta;
                for (uint32_t k = 0; k < p.opt.t6_iters; ++k) {
                    t = NowUs();
                    all.Resubmit();
                    ta.push_back(NowUs() - t);
                }
                uint64_t bad = 0;
                for (uint32_t i = 0; i < n; ++i) {
                    bad +=
                        CpuVerify(p.gm.Gva(region_va + uint64_t(i) * obj), 1, seed_all).mismatches;
                }

                // 5. Destroy cost (all GPU work above has completed).
                const double d0 = NowUs();
                for (auto& o : objs) {
                    im->Destroy(o);
                }
                const double destroy_us = NowUs() - d0;
                objs.clear();

                p.r.Add(test, variant, bad ? Status::Fail : Status::Info,
                        bad ? Sprintf("%llu objects did not receive the touch-all write",
                                      static_cast<unsigned long long>(bad))
                            : Sprintf("all %u objects received the touch-all write (%s)", n,
                                      PathName(path)),
                        {{"objects", double(n)},
                         {"create_import_total_us", create_us},
                         {"create_import_per_object_us", create_us / n},
                         {"first_submit_wait_after_import_us", first_submit_us},
                         {"empty_submit_wait_median_us", Median(te)},
                         {"tiny_dispatch_submit_wait_median_us", Median(tt)},
                         {"touch_all_first_submit_wait_us", touch_first_us},
                         {"touch_all_submit_wait_median_us", Median(ta)},
                         {"destroy_total_us", destroy_us},
                         {"destroy_per_object_us", destroy_us / n}});
            });
        }
    }
}

} // namespace

void RunPlanB(Probe& p) {
    if (!p.opt.Enabled("t4b") && !p.opt.Enabled("t5b") && !p.opt.Enabled("t6b")) {
        return;
    }
    p.r.Section("plan-B tests (ordinary imported VkBuffer)");
    const auto importers = MakeImporters(p);
    for (const auto& im : importers) {
        const std::string why = im->Unavailable();
        p.r.Fact(std::string("importer.") + im->Name(),
                 why.empty() ? "available" : "<" + why + ">");
    }
    if (p.opt.Enabled("t4b")) {
        for (const auto& im : importers) {
            T4bStress(p, *im);
        }
    }
    if (p.opt.Enabled("t5b")) {
        for (const auto& im : importers) {
            T5bRemap(p, *im);
        }
    }
    if (p.opt.Enabled("t6b")) {
        T6bScaling(p, importers);
    }
}

} // namespace e0b
