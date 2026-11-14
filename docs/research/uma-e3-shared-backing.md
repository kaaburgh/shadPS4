<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# E3 slice: buffers served from the guest physical backing (opt-in)

Branch: `research/uma-e3-shared-backing`, fork kaaburgh/shadPS4.
Base: E1B `3f70a6a` ([E1B notes](uma-e1b-scalar-completion.md)) plus three fixes below it:

- the two lavapipe patches from shadps4-open-test `patches/shadps4/`: EOP INT_SEL=3 in the legacy
  helper, and sparse arenas sized within maxBufferSize / sparseAddressSpaceSize;
- `scalar_completion: admit EOP INT_SEL=3`. E1B rejected that selector with UNREACHABLE. OpenGNM
  emits it for every label, so every open-test workload aborted on its first EOP.

The review order is E1 → E2 (baseline performance) → E3, and the E1B notes ask for E1C
(GDS / packet-specific visibility) before shared backing. This slice was started ahead of
that order, by decision of the project owner, because the open-test suite checks shared
backing directly. It covers the paths that guest buffer workloads use. The E1C items it does
not need are listed under "Open" and stay open.

## Mechanism

**Import path.** [SharedBacking](../../src/video_core/buffer_cache/shared_backing.h)
exposes the canonical guest physical backing through ordinary Vulkan buffers with BDA (E0b
Plan B). On x86-64 Linux RADV, the opt-in allows sealing the backing memfd. After sizing the
file, AddressSpace applies `F_SEAL_SHRINK` and keeps using its canonical `MAP_SHARED` mapping.
`UDMABUF_CREATE` exports one page-aligned, contiguous memfd range per chunk. The kernel
may back that range with scattered system pages. Other drivers keep the existing 256 MiB
`VK_EXT_external_memory_host` path.

- RADV needs `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf` and BDA. The
  importer checks external-buffer properties for the actual buffer usage, queries fd and
  buffer memory types, and chooses a `HOST_VISIBLE|HOST_COHERENT` type from the intersection.
  It binds the entire chunk at buffer memory offset zero. Vulkan owns the fd after successful
  `vkAllocateMemory`; the caller closes it on allocation failure.
- DMA-BUF chunks are at most 16 MiB and are rounded down to host pages after applying the
  runtime udmabuf and Vulkan size limits. The 64 MiB session budget allows four full chunks
  on the M75n. It is under 2.2% of the reported 2931 MiB GTT and about 1.1% of system RAM.
  This cap bounds extra pinning when the emulator already uses the remaining RAM. Chunks
  import on demand and stay alive until GPU work drains at renderer teardown.
- Guest VAs resolve to physical offsets. Aliases in shared ranges see the same bytes.
  Unmap drops the VA's BDA entries without destroying the chunk, and a remap resolves its own
  physical offset. This also keeps resources alive for GPU work already recorded or submitted.
- The host accesses the original memfd mappings. On the tested x86-64 RADV/udmabuf stack,
  coherent memory plus the E3 device-to-host barrier and timeline wait sufficed in E0b T4b
  and T5b without `DMA_BUF_IOCTL_SYNC`. Linux specifies that ioctl for CPU access via a
  mapping of the DMA-BUF fd. It provides cache maintenance and cannot replace GPU completion.
  The importer uses the original mappings and the existing E3 ordering.

**Which requests are shared.** Shared state is kept per block. A block is the BDA page-table
granularity: the sparse alignment, at least 16 KiB (16 KiB on lavapipe).
`ObtainBuffer` serves a request from the backing when all of these hold for its block-aligned
range:

- it maps to one contiguous piece of backing, per `MemoryManager::GetContiguousBacking` over
  the 16 KiB backing page table;
- it lies in one chunk;
- none of its blocks already has arena memory.

It then returns the chunk buffer and offset, and points the blocks' BDA entries into the chunk.

**What shared blocks skip.**

- No stream copy, and no upload. No region manager is created for them, so they are never
  write-protected.
- No GPU-modified marking and no download. They are never read-protected. The guest sees GPU
  writes after completion in every readbacks mode.
- The emulator's own record-time reads are not protected either: sharps, user data,
  `IsComputeMetaClear`. In Precise mode the mirror ran `Finish` before such a read; shared
  blocks do not (see Open, E1C).

**Mirror fallback.** The rest keeps the mirror path.

- A block with arena memory stays mirrored for the rest of the session: `resident_ranges`
  never shrinks.
- A mirror request that touches shared blocks demotes them. It runs `Finish`, which lets
  recorded writes reach guest memory, then marks the blocks CPU-modified. `EnsureResident`
  then rewrites their BDA entries to point at the arena.

**Guards elsewhere.** They keep the paths below in GPU order. The CP packets listed under
Open still access guest memory while parsing.

- **Unmap.** `Rasterizer::UnmapMemory` posts a CP command that forgets the shared blocks and
  zeroes their BDA entries. The guest does not wait for it: the CP runs commands before it
  parses the next packet. The next use resolves the new physical memory.
- **Image sources.** `ObtainBufferForImage` returns the shared buffer, so an image is filled by
  a GPU copy instead of a record-time CPU copy. Texel reads of images or metadata stay
  mirrored, because they write the image into the buffer.
- **DmaData.** The fill/copy CPU fast paths skip destinations that are shared or could be
  shared; those are filled and copied on the GPU timeline. This also covers a second VA of
  physical memory that recorded work uses through the first, as long as the second VA itself
  can be shared. A copy from a shared or shareable source into a destination that cannot be
  shared waits for recorded work (`Finish`) and then copies in guest memory: a GPU copy would
  leave the result in the destination's mirror, which readbacks Disabled/Relaxed never return
  to the guest.
- **WaitRegMem.** While the predicate fails, the CP submits newly recorded work before each
  yield. A shader writes guest memory only after its submit, and the producer may be recorded
  by another queue while this one waits.
- **Visibility.** While shared backing is enabled, every submit ends with an
  ALL_COMMANDS/MEMORY_WRITE → HOST/HOST_READ|HOST_WRITE memory barrier. A shader can reach
  shared blocks through BDA without obtaining a buffer in that submit. The barrier sits right
  before the timeline signal, which waits for all work of the submit anyway.

**Opt-in.** Set `SHADPS4_UMA_SHARED_BACKING=1`. If the selected importer, device
node, sealing or kernel limit is unavailable, the emulator uses mirrors. A chunk whose
import fails or exceeds the pin budget is remembered and uses the mirror. A failed first
import disables shared backing for the session. Requests crossing chunk boundaries
also fall back. The log reports the backend, import results, memory types and live bytes.
A first shared BufferCache request log and the Buffer census events confirm actual use.
The shutdown summary counts shared requests, fallback requests attempted while shared
backing was active, and demotion stalls. A runner that stops the process at the guest
marker may end before teardown summaries are flushed.

RADV on the M75n exposes sparse binding on a dedicated queue family. Existing sparse
mirror arena binds use that queue, and rendering waits on the bind timeline semaphore.
This applies with the opt-in disabled as well. When the graphics queue supports sparse
binding it remains the selected queue.

## Dependency on E1B

With shared backing, the guest reads GPU results directly: there is no readback and no
`Finish`. That is only correct if guest-visible completion (EOP labels and IRQs) appears after
the host GPU has finished. That is E1B's contract. The early-label control below removes it,
and the suite then fails.

## Validation

Environment:

- lavapipe, Mesa 25.2.8 (LLVM 20.1.2), Xvfb, 4 vCPU;
- [shadps4-open-test](https://github.com/kaaburgh/shadps4-open-test) `5693622`, all 11 tests;
- runner `scripts/run-test-lavapipe.sh`. Precise uses the isolated config; Disabled and
  Relaxed use host configs with `readbacksMode` 0 and 1.

| test | mirror Precise | mirror Disabled | shared Precise | shared Relaxed | shared Disabled |
|---|---|---|---|---|---|
| gpu_solid_rt | PASS | PASS | PASS | PASS | PASS |
| buffer_compute_roundtrip | PASS | FAIL | PASS | PASS | PASS |
| buffer_cpu_rewrite | PASS | FAIL | PASS | PASS | PASS |
| buffer_page_false_sharing | PASS | FAIL | PASS | PASS | PASS |
| buffer_pingpong | PASS | FAIL | PASS | PASS | PASS |
| buffer_granularity_matrix | PASS | FAIL | PASS | PASS | PASS |
| physical_alias | FAIL (kaaburgh/shadPS4#5) | FAIL | PASS | PASS | PASS |
| ordered_async_handoff | PASS | FAIL | PASS | PASS | PASS |
| image_buffer_alias | FAIL (kaaburgh/shadPS4#4) | FAIL | PASS | PASS | PASS |
| map_unmap_reuse | PASS | FAIL | PASS | PASS | PASS |
| uma_churn | PASS | FAIL | PASS | PASS | PASS |

**Result columns.**

- **Mirror.** E1B without the opt-in, measured both before and after this change, with
  identical results. Relaxed matches Disabled.
- **Shared.** Every checksum and digest equals the mirror Precise reference. The shared runs
  logged no mirror fallback and no `<Critical>` line.
- **After the review fixes** (arena rounding, host-coherent memory type, asynchronous unmap
  reset, DMA shareable check, WaitRegMem flush per poll, host barrier on every submit, turning
  off after a failed first import), all five columns were re-run with identical results. lavapipe still gets 1 GiB arenas; its one chunk is imported as memory type
  0, its only host-import type.

**Negative controls.** Temporary builds, not committed; shared backing on, Disabled mode.

| mutation | result |
|---|---|
| EOP labels published while parsing, before host completion (pre-E1B behavior) | 10/11 FAIL with unwritten outputs; only gpu_solid_rt (render-target readback) passes |
| `ObtainBufferForImage` copies shared regions on the CPU at record time | 11/11 PASS: the image-source guard is not exercised by the suite |

### Gate B on M75n (Linux RADV)

Baseline: `66da8f1cdc56e6dd57fbabb59195237046b543d9`. This is eight commits
ahead of `8d07f08`, including the cross-VMA `TryWriteBacking` fix `83584ce`.
Candidate tested: `b4946c18da14cfeb7215952a4c786edf67527030` (same source tree as
the initial Gate B PR commit `91bdcbbfd37cca6a55bda0e208b8319f191a0d9e`).
Guest suite: [`eb1f2fefb1afc01baa2e2fa15265c81135b1c8e9`][open-test-sha].
Both emulator builds use DietPi Clang 19.1.7, C++ `-O0` and C `-O1`.
The M75n runs Debian 13.7, Linux `6.12.117+deb13-amd64`, Mesa
`25.0.7-2+deb13u1` (RADV RAVEN), Vulkan loader/layers 1.4.309 and device
API 1.4.305. The guest runner uses headless Weston and Vulkan core and
synchronization validation.

Gate A ([PR #53][stage-one]) passed on both saved M75n reports. A fresh
validated run of the pinned E0b probe `e35713c` reproduced four Plan B
passes (T4b/T5b, BDA/SSBO) and zero validation errors. The probe's exit 1
is explained by the four optional host-pointer import failures. This is
the host feasibility gate.

| Guest workload | E3 mirror | New mirror | DMA-BUF Precise | DMA-BUF Disabled |
| --- | --- | --- | --- | --- |
| `buffer_compute_roundtrip`, `buffer_cpu_rewrite`, `buffer_pingpong` | PASS | PASS | PASS | PASS |
| `physical_alias` | FAIL #5 | FAIL #5 | PASS | PASS |
| `map_unmap_reuse`, `uma_churn` | PASS | PASS | PASS | PASS |
| `dma_order`, `dma_mixed_endpoints` | PASS | PASS | PASS | PASS |
| `fallback_in_shared` | PASS | PASS | PASS | R (mirror readback) |
| `dma_alias_order` | FAIL #5 | FAIL #5 | FAIL #8 | FAIL #8 |
| `write_data_order`, `cond_exec_gpu_predicate` | PASS | PASS | FAIL #11 | — |

With stock RADV, the original E3 binary used the wrong sparse queue
(`VUID-vkQueueBindSparse-queuetype`, failed compute round-trip). The E3
mirror column uses process-local `radv_legacy_sparse_binding=true`. The
candidate used stock RADV. The exact E3 binary with lavapipe host-pointer
backing reproduced all three shared-path guest failure markers and values,
including both noncontiguous `dma_alias_order` cases. In `fallback_in_shared`
with readbacks Disabled, the shared control passed and mirrored outputs
were non-diagnostic.

RADV core/sync validation reported two pre-existing fingerprints
(`VUID-VkPipelineShaderStageCreateInfo-pNext-02755` and
`SYNC-HAZARD-WRITE-AFTER-PRESENT`). The candidate added no fingerprints,
device loss, pre-marker crashes or timeouts in these runs.

All 22 opt-in matrix processes logged real DMA-BUF and a shared BufferCache
request. There were 13 imports in 12 Precise processes and 11 imports in 10
Disabled processes. Every chunk was 16 MiB; the fd and buffer masks were
`0x420` and `0x7ad`, and the allocator selected type 5. Removing the
temporary `kvm` group kept the render node accessible but denied
`/dev/udmabuf`. Both selected guest controls passed via the mirror with zero
imports. The candidate also passed compute round-trip and physical aliases
with the host-pointer backend on lavapipe.

A bounded extension of `map_unmap_reuse` used five different physical ranges
at the same guest VA. The fourth import reached 64 MiB, the fifth was refused
by the pin budget, and the guest oracle still returned checksum `d07f69c5`
with Precise readbacks (baseline and both candidate modes).
The census recorded 30 shared and 6 mirror requests in the DMA-BUF run.
Repeated runs peaked at 117.5 MiB GTT with DMA-BUF and 69.5 MiB in mirror,
with sampled process RSS of about 314–315 MiB and 315–322 MiB respectively.
`VmLck` and `VmPin` reported zero; they do not account for all driver pins.
A separate T6b host run with four 16 MiB chunks saw GTT rise by about
64.5 MiB and return to baseline after release. The few short mixed startup
and execution timings do not establish a performance gain.

Guest results are bounded to these configurations. The runner terminates
at the marker, so it does not verify graceful shutdown. The focused test
covers remap with completed dispatches, and `uma_churn` covers pending
writes through same-physical remaps. Different-physical remaps under
in-flight writes, demotion reentry and general mixed alias ordering
remain open. There is no larger-chunk, large-workload, or NVIDIA result.

[open-test-sha]: https://github.com/kaaburgh/shadps4-open-test/tree/eb1f2fefb1afc01baa2e2fa15265c81135b1c8e9
[stage-one]: https://github.com/kaaburgh/shadPS4/pull/53

## Open

- **Paths the suite does not reach.** These guards follow from the design but no test can fail
  without them:
  - the image-source guard;
  - the WaitRegMem submit;
  - the unmap BDA reset (descriptors re-resolve the physical address on every use);
  - demotion of shared blocks;
  - DMA/BDA shader accesses.

  Each needs a dedicated workload (kaaburgh/shadPS4#13): a GPU-written WAIT_REG_MEM value, a
  BDA/flat-memory shader across a remap, a request straddling contiguous and non-contiguous
  mappings, and an image sourced from memory a dispatch writes in the same submit.
- **E1C packets** (kaaburgh/shadPS4#11). The CP still accesses guest memory while parsing,
  out of GPU order for shared memory:
  - writes: `WRITE_DATA`, `DUMP_CONST_RAM`, ZPassDone, `MEM_SEMAPHORE`. `WRITE_DATA` regresses
    against the mirror: a dispatch recorded before the packet now reads the new value;
  - reads: `COND_EXEC`, `REWIND`, `INDIRECT_BUFFER` contents, sharps and user data;
  - Release-GDS publication and DmaData cp_sync/raw_wait remain as in E1B.

  EOP and `RELEASE_MEM` labels are not affected: E1B publishes them after host completion.
- **Demotion** (kaaburgh/shadPS4#7). It runs `Finish` inside `ObtainBuffer`. From a FaultManager
  deferred operation this re-enters the scheduler and hangs; during draw binding it loses the
  vertex input and the vertex/index bindings already recorded for the draw. It needs a mirror
  request over an already shared block, which the suite never makes.
- **Physical aliases** (kaaburgh/shadPS4#8). Shared state is tracked per VA. After a fallback,
  one alias can be mirrored while another stays shared. A DMA through an alias that cannot be
  shared still takes the CPU fast path.
- **Textures.** Images and render targets stay in the TextureCache mirror; only buffers are
  shared.
- **Fallbacks** (kaaburgh/shadPS4#12).
  - A range that crosses a 256 MiB physical chunk boundary uses the mirror.
  - So does any range whose blocks are not physically contiguous: shadPS4 maps guest memory
    in 16 KiB pieces. Stitched imports (`host_gva` / udmabuf lists from E0b) are not
    implemented.
  - Unmapping part of a 64 KiB BDA block drops the whole block, including mapped neighbours.
- **Hardware.**
  - NVIDIA: plan-B host import of the backing mapping passed E0b, but this slice has not run
    there. Pinning of whole chunks and PCIe cost need measuring (kaaburgh/shadPS4#10).
  - AMD/RADV needs the udmabuf + dma-buf importer, which is not implemented
    (kaaburgh/shadPS4#9).
  - Nothing has been measured. lavapipe is not a performance model, and each shared request
    walks the page table at 16 KiB steps.
- **Remap and lifetime.** Imports are physical, so a guest remap leaves no stale Vulkan
  object. As on hardware, the guest must not unmap memory that queued GPU work still uses.
