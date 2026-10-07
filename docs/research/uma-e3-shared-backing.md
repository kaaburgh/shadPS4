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
- Imports are 256 MiB chunks created on first use. Each chunk is one plain buffer with BDA.
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
- No GPU-modified marking and no download. They are never read-protected, so the readbacks
  mode does not matter for them.

**Mirror fallback.** The rest keeps the mirror path.

- A block with arena memory stays mirrored until the guest unmaps it.
- A mirror request that touches shared blocks demotes them. It runs `Finish`, which lets
  recorded writes reach guest memory, then marks the blocks CPU-modified. `EnsureResident`
  then rewrites their BDA entries to point at the arena.

**Guards elsewhere, so nothing reads or writes shared memory at record time out of GPU order:**

- **Unmap.** `Rasterizer::UnmapMemory` posts a CP command that forgets the shared blocks and
  zeroes their BDA entries. The next use resolves the new physical memory.
- **Image sources.** `ObtainBufferForImage` returns the shared buffer, so an image is filled by
  a GPU copy instead of a record-time CPU copy. Texel reads of images or metadata stay
  mirrored, because they write the image into the buffer.
- **DmaData.** The fill/copy CPU fast paths skip shared regions; those regions are filled and
  copied on the GPU timeline.
- **WaitRegMem.** If the predicate fails, recorded work is submitted once before the CP yields.
  A shader writes guest memory only after its submit.
- **Visibility.** Each submit that used shared buffers ends with an
  ALL_COMMANDS/MEMORY_WRITE → HOST/HOST_READ|HOST_WRITE memory barrier.

**Opt-in.** Set `SHADPS4_UMA_SHARED_BACKING=1`; the device must support
`VK_EXT_external_memory_host`. A chunk whose import fails is remembered and its requests use
the mirror. RADV is expected to fail here, because amdgpu userptr refuses file-backed memory.
Without the variable the emulator behaves as E1B; the opt-out runs below show that.

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

  Each needs a dedicated workload: a GPU-written WAIT_REG_MEM value, a BDA/flat-memory shader
  across a remap, a request straddling contiguous and non-contiguous mappings, and an image
  sourced from memory a dispatch writes in the same submit.
- **E1C packets.**
  - WriteData and the CP-side `TryWriteBacking` stores still write guest memory while parsing.
    With shared backing such a store can now be seen by recorded but unsubmitted GPU work.
  - Release-GDS publication, DmaData cp_sync/raw_wait and MemSemaphore remain as in E1B.
- **Textures.** Images and render targets stay in the TextureCache mirror; only buffers are
  shared.
- **Fallbacks.**
  - A range that crosses a 256 MiB physical chunk boundary uses the mirror.
  - So does any range whose blocks are not physically contiguous: shadPS4 maps guest memory
    in 16 KiB pieces. Stitched imports (`host_gva` / udmabuf lists from E0b) are not
    implemented.
- **Hardware.**
  - NVIDIA: plan-B host import of the backing mapping passed E0b, but this slice has not run
    there.
  - AMD/RADV needs the udmabuf + dma-buf importer, which is not implemented.
  - Nothing has been measured. lavapipe is not a performance model, and each shared request
    walks the page table at 16 KiB steps.
- **Remap and lifetime.** Imports are physical, so a guest remap leaves no stale Vulkan
  object. As on hardware, the guest must not unmap memory that queued GPU work still uses.
