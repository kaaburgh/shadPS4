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

**Import path.** The E0b probe established that neither lavapipe nor the tested RTX 5070 Ti
accepts imported memory in a sparse arena, while a plain buffer over an import works on both
("plan B"). So:

- [SharedBacking](../../src/video_core/buffer_cache/shared_backing.h) imports the canonical
  backing mapping (`AddressSpace::BackingBase`, the `MAP_SHARED` memfd view) with
  `VK_EXT_external_memory_host`.
- Imports are 256 MiB chunks created on first use. Each chunk is one plain buffer with BDA,
  imported with a `HOST_VISIBLE|HOST_COHERENT` memory type: the host keeps accessing the
  backing through its own mapping, without flush or invalidate. A chunk with no such type
  counts as failed.
- Indexing is by physical address. An import therefore stays valid when the guest remaps a
  VA, and two guest aliases of one physical page reach the same bytes.

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

**Opt-in.** Set `SHADPS4_UMA_SHARED_BACKING=1`; the device must support
`VK_EXT_external_memory_host`. A chunk whose import fails is remembered and its requests use
the mirror. If the very first import fails, shared backing turns itself off: nothing is shared
yet, so the emulator then behaves as without the variable. That is the expected outcome on
RADV, where libdrm creates userptr BOs with `AMDGPU_GEM_USERPTR_ANONONLY` and amdgpu refuses
file-backed memory, until a udmabuf importer exists (kaaburgh/shadPS4#9).

Without the variable the emulator behaves as E1B, except for the arena size from the lavapipe
patch. Arenas stay 4 GiB unless the device reports a sparse address space below 8 GiB or a
maxBufferSize below 4 GiB. maxBufferSize is rounded up to whole MiB, so RADV (4 GiB - 4) keeps
4 GiB arenas, while lavapipe (2 GiB sparse address space) gets 1 GiB. The opt-out runs below
show the E1B behaviour.

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
