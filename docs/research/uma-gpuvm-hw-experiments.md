<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# GPUVM on hardware: imported, shifted and stitched dma-buf memory in a sparse buffer on RADV

Follows the [PR #54 architecture review][pr54], the [probe of PR #56][pr56] and the
[granularity note of PR #57][pr57]. Those left three things as "unknown on every driver":
whether imported guest memory bound with `vkQueueBindSparse` aliases the guest pages, whether
the same pages can be imported several times at different offsets, and whether one memory
object stitched from non-adjacent pages can be bound. This note records the experiments that
answer them on one machine.

**Scope.** Everything measured here comes from one machine and one driver build: the M75n,
AMD Radeon Vega 6 (RADV RAVEN), Mesa 25.0.7, Linux 6.12.117. Nothing was run on NVIDIA, on a
discrete AMD card or on Mesa 26.2. The production UMA backend is not changed, and these probes
alone do not decide the GPUVM architecture (section 7).

## Summary

1. **Gate: passes.** S1 to S3 of the #56 probe pass on imported dma-buf memory, plain and
   under the validation layer with no message. Binding imported guest memory into a sparse
   "guest VA" buffer aliases the guest pages for reads, writes and remaps.
2. **Shifted imports: pass.** The same guest pages imported at phases 0, 16, 32 and 48 KiB and
   bound into one arena show the same bytes. A write through any import is seen through every
   other one and by the CPU, across submissions and inside one command buffer.
3. **Stitched imports: pass.** One memory object over non-adjacent guest pages
   (`UDMABUF_CREATE_LIST`) binds as whole 64 KiB blocks and stays consistent with ordinary
   imports of the same pages in both directions. A partial unmap can be made exact by swapping
   in a new object with one rebind.
4. **A premise changed.** RADV does not report a 64 KiB sparse block on this machine. It
   reports 4 KiB, so the emulator's block is 16 KiB, equal to the guest page, and the
   16 KiB against 64 KiB mismatch of the granularity note does not exist here. By the Mesa
   source it starts to exist with Mesa 26.2, which reports 64 KiB unconditionally
   (section 2). All tests were therefore also run with RADV told to report 64 KiB.
5. **Cost found.** Every import is charged at its full size against the GTT counter once a
   submission uses it, however little of it is bound. Four phases over the same memory cost
   four times its size (section 5).

## 1. How to repeat

```sh
tools/uma_gpuvm_probe/run_probe.sh OUT_DIR all
```

The script needs a C compiler, the Vulkan headers and loader, and read/write access to
`/dev/udmabuf`. It takes under a minute. It builds the probe from the sources next to it, runs
every test with the driver as installed and with `radv_force_64k_sparse_alignment=true`, plain
and under `VK_LAYER_KHRONOS_validation`, and writes into `OUT_DIR`: the sources used, the
environment (kernel, compiler, package versions, udmabuf limits, git commit), one log per run,
`summary.txt` and `SHA256SUMS`. It exits 0 when no run failed. `PROBE_DEVICE` selects the
Vulkan device (0 on the M75n; device 1 is llvmpipe).

Results kept in this repository, each directory complete except for the binary:

| Directory under `docs/research/gpuvm-hw-results/` | Probe commit | What ran | Result |
| --- | --- | --- | --- |
| `gate-20261116T233744Z` | `8db9ce0` | gate only (S1 to S3, E1) | 4 runs passed |
| `all-20261116T235828Z` | `ee5219f` | gate, T, U, canary, two driver configurations | 22 passed, 4 skipped, 0 failed |
| `all-20261117T000322Z` | `08509bf` | the same plus T over 16 MiB with GTT readings | 28 passed, 6 skipped, 0 failed |

The skipped runs are the controls on driver memory under the forced 64 KiB granularity. With a
64 KiB granularity one driver allocation cannot express a 16 KiB phase, which is the point of
the experiment, so those controls have nothing to run. `SHA256SUMS` of the first two
directories was written by an earlier version of the script: in the gate directory it does not
cover the last line of `summary.txt`, and in the second it lists the binary, which is not
committed. The third directory verifies as it is.

Machine: Lenovo M75n, AMD Radeon Vega 6 (RADV RAVEN), Mesa 25.0.7-2+deb13u1, Vulkan 1.4.305,
validation layers 1.4.309.0, Linux 6.12.117+deb13-amd64, Debian 13, cc 14.2.0. udmabuf limits
at their defaults: `list_limit` 1024, `size_limit_mb` 64. GTT size 3074285568 bytes.

## 2. The sparse block RADV reports

| Driver | Sparse buffer alignment | Source |
| --- | --- | --- |
| RADV, Mesa 25.0.7 (M75n) | 4 KiB | measured: `block=4 KiB` in every `default` log |
| RADV, Mesa 25.0.7 with `radv_force_64k_sparse_alignment=true` | 64 KiB | measured: `block=64 KiB` in every `force64k` log |
| RADV, Mesa 25.1 to 26.1 | 4 KiB, the option no longer exists | read in the Mesa source, not run |
| RADV, Mesa 26.2.0 and later | 64 KiB, unconditional | read in the Mesa source, not run: `RADV_SPARSE_BUFFER_ALIGNMENT (64 * 1024)`, commit `cb8da4bb419a` "radv: bump the sparse alignment requirement to 64K" |

The emulator computes its block as `AlignUp(max(alignment, 16 KiB), alignment)`
(`buffer_cache.cpp:91`). On the M75n today that is 16 KiB, so a sparse arena can represent
every backed guest map exactly with the imports PR #54 already has, and neither shifted nor
stitched imports are needed. Issue #12 and section 2.2 of the granularity note assumed 64 KiB
for RADV. That becomes true when the machine gets Mesa 26.2 or later. The change is a driver
policy (its commit message gives TLB cost as the reason), not a kernel or hardware limit: the
same kernel binds 4 KiB units here.

The forced configuration is how the 64 KiB rule was tested without Mesa 26.2. It changes what
the driver reports, and the validation layer then checks every bind against 64 KiB (section
3.3). It does not prove that Mesa 26.2 executes binds the same way.

## 3. Tests

All tests use 16 KiB guest pages, a test block of 64 KiB and a small "guest memory" held in a
memfd. Every word of every page carries its page number, so a read through a wrong page is
seen. After each step the whole arena is copied to a readback buffer and compared word by word
with a model, and the memfd is compared with the model through the CPU mapping.

### 3.1 Shifted imports (T1 to T6)

Guest memory is imported four times through udmabuf, starting at page 0, 1, 2 and 3. Import
`p` makes guest page `n` available at a memory offset that is a 64 KiB multiple when
`n ≡ p (mod 4)`. Arena block `p` is bound to guest pages `16 + p .. 19 + p` through import
`p`, so guest page 19 is visible through all four imports at four different places. Two more
blocks hold a 128 KiB mapping with a 16 KiB phase, which is counterexample CE1 of the
granularity note.

| Test | Checks |
| --- | --- |
| T1 | all views, the two-block mapping and both controls read the expected bytes |
| T2 | a GPU fill of guest page 19 through each phase in turn; after each one, a read of every view in a separate submission, and the memfd through the CPU |
| T3 | negative control: an import of other pages that started with the same bytes shows none of the writes (4096 of 4096 words differ) |
| T4 | as T2 with fill, memory barrier and read in one command buffer |
| T5 | CPU writes to the memfd are read by the GPU through all views |
| T6 | a GPU fill across the block boundary of the CE1 mapping lands on two neighbouring guest pages |

Positive control: arena block `r + 2` is a second bind of the same object and offset as block
0, the aliasing Vulkan defines. The cross-object views have to behave like it.

### 3.2 Stitched imports (U1 to U7)

One memory object is made with `UDMABUF_CREATE_LIST` from 12 guest pages in 7 list items and
bound as three whole blocks: one whose four pages come from four different places and phases
in descending order (pages 81, 68, 59, 46), one that joins two physical runs in the middle of
the block (CE2), and one of consecutive pages as positive control. Ordinary imports of some of
the same pages are bound next to it.

| Test | Checks |
| --- | --- |
| U1 | all blocks read the expected bytes |
| U2 | a GPU fill through each of the 8 irregular pages lands on its guest page, seen by the CPU and through the ordinary view |
| U3 | GPU fills through the ordinary views are read through the stitched object |
| U4 | negative control, as T3 (16384 of 16384 words differ) |
| U5 | fill, barrier and read in one command buffer, in both directions |
| U6 | exact partial unmap (CE4): a new stitched object with a scratch page in place of guest page 68 is swapped in with one rebind; a GPU fill of the whole block then leaves page 68 untouched |
| U7 | information: the kernel accepts a list that names one guest page twice |

### 3.3 Controls on the method

- **Driver-memory control.** `shifted-control` and `stitched-control` build the same arena
  layout from one driver allocation. They pass on RADV at 4 KiB and on lavapipe, so the test
  logic and the model are sound independently of imports.
- **Validation canary.** A silent validation layer and an absent one look the same, so each
  run starts by breaking rules on purpose. V1 creates a buffer without usage. V2, only where
  RADV was told to report 64 KiB, binds 16 KiB at a 16 KiB offset. The layer reported both:
  `VUID-VkBufferCreateInfo-None-09500`, and `VUID-VkSparseMemoryBind-resourceOffset-09491`
  once each for `resourceOffset`, `memoryOffset` and `size`, plus
  `VUID-VkSparseMemoryBind-memory-01096`. So in the `force64k` runs the layer does check
  binds against 64 KiB, and its silence in the T and U runs means every bind was a legal
  64 KiB bind.
- **Probe self-check.** `add_bind` refuses any bind whose offsets or size are not multiples
  of the reported granularity, before the driver sees it.

## 4. Results on the M75n

Run `all-20261117T000322Z`. "ok" is all tests of the run passed with no validation message.

| Run | default (4 KiB), plain | default, validation | forced 64 KiB, plain | forced 64 KiB, validation |
| --- | --- | --- | --- | --- |
| canary V1, V2 | n/a | V1 reported, V2 not applicable | n/a | V1 and V2 reported |
| control S1 to S3, E1 | ok | ok | ok | ok |
| dma-buf S1 to S3, E1 | ok | ok | ok | ok |
| shifted control T1 to T6 | ok | ok | skipped | skipped |
| shifted dma-buf T1 to T6 | ok | ok | ok | ok |
| stitched control U1 to U6 | ok | ok | skipped | skipped |
| stitched dma-buf U1 to U6 | ok | ok | ok | ok |
| shifted dma-buf over 16 MiB | ok | ok | ok | ok |

Every content check reported 0 mismatches. `dmesg` showed no amdgpu or udmabuf message during
the second run.

## 5. Costs measured

**GTT accounting.** `mem_info_gtt_used` was read before the imports, after the bind and after
the first submission that used the arena.

| Case | After the bind | After the first GPU use |
| --- | --- | --- |
| shifted, 16 MiB guest memory, 5 imports of 16711680 bytes, 8 blocks bound | +0 | +83902464 |
| the same layout on driver memory (allocated before the first reading) | +0 | +344064 |
| stitched: one object of 12 pages, two imports of 124 pages | +0 | +4603904 |

83902464 is exactly 5 × 16711680 + 344064, and 4603904 is exactly
196608 + 2 × 2031616 + 344064. So each import is charged at its full size as soon as one
submission uses the arena it is bound into, although one or two 64 KiB blocks of it are bound.
The pages are the same guest pages every time, so this is accounting against the GTT limit,
not extra RAM. With whole-chunk imports at all four phases the charge is four times the guest
memory, against a GTT of 2.86 GiB on this machine. PR #54 measured the single charge before
(+64.5 MiB for 4 × 16 MiB).

**Time.** One measurement per run, not a benchmark.

| Operation | Time |
| --- | --- |
| bind 16 MiB as 4096 separate 4 KiB binds, one call | 205 to 374 ms across runs |
| bind 16 MiB as 256 separate 64 KiB binds, one call (forced 64 KiB) | 3.7 to 5.0 ms |
| rebind of one block, call plus queue idle | 0.06 to 0.46 ms |
| bind of the 8 blocks of the shifted test | 0.20 to 0.25 ms |
| `UDMABUF_CREATE_LIST` for 16 MiB of pages | 0.33 to 0.80 ms |
| `vkAllocateMemory` import of that dma-buf | 0.14 to 0.18 ms |
| stitched object of 12 pages: udmabuf plus import | about 0.1 ms |
| swap of a stitched object after a partial unmap: rebind | 0.08 to 0.14 ms |

The 16 KiB bind size the emulator would use on this driver was not timed.

## 6. Hypotheses

### Refuted

1. **"RADV has a 64 KiB sparse block."** Not on Mesa 25.0.7: it is 4 KiB, and by the source it
   stays 4 KiB through Mesa 26.1. The statement holds from Mesa 26.2. So the granularity
   mismatch on RADV depends on the Mesa version.
2. **"RADV may accept imported memory in a sparse bind without aliasing it, as lavapipe
   does."** It aliases: S1 to S3 pass with content checks in both directions.
3. **"A second import of the same pages may be refused, or the kernel may refuse overlapping
   udmabufs."** Four overlapping imports of one memfd range, and a list with the same page
   twice, were all accepted.
4. **"A block whose pages are not one physical run can only be served by a copy (mirror or
   gather)."** On this driver a stitched object served such blocks (CE2, CE4) without a copy,
   and shifted imports served mappings with a phase (CE1, CE3), with the driver reporting
   64 KiB and the validation layer checking it.

### Confirmed by experiment (M75n, RADV, Mesa 25.0.7, Linux 6.12)

1. Imported udmabuf memory bound sparsely aliases the guest pages for GPU reads, GPU writes
   and rebinds. Layer 3 of the PR #57 recommendation can exist on RADV.
2. Several imports of the same pages at 16 KiB phases stay consistent with each other and with
   the CPU, for transfer commands, in separate submissions and within one command buffer with
   a global memory barrier.
3. A `UDMABUF_CREATE_LIST` object over non-adjacent pages can be imported and bound, and is
   consistent with ordinary imports of the same pages in both directions.
4. A partial unmap inside a bound block can be made exact with a new stitched object and one
   rebind of the block.
5. All of the above also hold when RADV reports 64 KiB, with every bind validated against that
   granularity.
6. Driver-owned backing exported as a dma-buf and mapped by the CPU in permuted 16 KiB pieces
   works (E1), which keeps the inverse-ownership option open on RADV.
7. Imports are not free: each one is charged at its full size against GTT from its first use,
   so the charge grows with the number of phases imported (section 5).

### Still open

1. **Mesa 26.2 and later.** The 64 KiB case was produced by an option of Mesa 25.0, not by the
   driver that has it as default. Debian backports offer Mesa 26.1.6 for this machine, which
   by the source still reports 4 KiB.
2. **NVIDIA.** Nothing here applies to it. NV-5 (the gate) is still queued. The shifted and
   stitched tests exist for dma-buf only, and NVIDIA needs host-pointer variants (NV-7, NV-8).
3. **Shader access.** The tests use `vkCmdFillBuffer` and `vkCmdCopyBuffer`. Access from
   shaders through device addresses of two aliases, and from graphics pipelines, was not
   tested.
4. **Concurrency.** Binds ran with idle queues. Rebinding a block while other submissions use
   the arena, and the cost of the quiesce that section 6 of the granularity note requires,
   were not tested.
5. **Scale.** The largest case is 16 MiB and five imports. Unknown: thousands of memory
   objects, multi-GiB imports against the GTT limit, the udmabuf limits (64 MiB per object and
   1024 list items by default), and what a real title's map and unmap rate does to bind time.
6. **Bind time at 16 KiB**, and the TLB cost that made Mesa move to 64 KiB.
7. **Lifetime.** What a live import sees when the guest unmaps or remaps the underlying memfd
   range. udmabuf pins the pages, so the expected answer is "the old pages", untested.
8. **Incidence in real titles.** Whether mappings that need shifted or stitched imports occur
   in bytes that matter is a question for `gpuvm_model.py census` on E0 captures (PR #57), not
   for these probes.
9. **Other AMD hardware and kernels.** One APU, one kernel.

## 7. What this changes, and what it does not

These results change inputs of the comparison in section 7 of the granularity note. They do
not choose between its options.

- The "unverified dependencies" of the sparse arena (sparse plus import, shifted imports,
  stitched imports) are verified on RADV for transfer commands. They remain unverified on
  NVIDIA, and plan B depends on NVIDIA.
- On RADV up to Mesa 26.1 the arena needs none of the extra machinery, because the block
  equals the guest page. From Mesa 26.2 it needs shifted or stitched imports for the mappings
  of CE1 to CE4. A design that relies on the 16 KiB block therefore has a known expiry date on
  Linux.
- Shifted imports are not free. Whole-chunk imports at every phase multiply the GTT charge by
  the number of phases, so a design that uses them has to import per mapping, or raise the
  GTT limit, or keep the number of phases in use small. Which of these is acceptable depends
  on the census data.
- Nothing here measures the per-access path, the cost under a running title, or correctness
  under concurrent rebinds. Recommendation 1 and 2 of the granularity note (chunk buffers as
  the base layer, 16 KiB translation table) do not depend on any result of this note.

[pr54]: https://github.com/kaaburgh/shadPS4/pull/54
[pr56]: https://github.com/kaaburgh/shadPS4/pull/56
[pr57]: https://github.com/kaaburgh/shadPS4/pull/57
