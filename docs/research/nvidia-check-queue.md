<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# NVIDIA check queue

The NVIDIA host (Ryzen 9 7945HX3D + RTX 5070 Ti, Linux) is available about once a day.
M75n (Vega 6, RADV) is always available and is checked right away. Checks that only
NVIDIA can answer collect here and run in one batch per session. Nothing else waits on them.

How to use:

- Add an item when a change needs NVIDIA evidence. Give it the next `NV-` number.
- Status is one of `queued`, `blocked` (say on what), `done: PASS`, `done: FAIL`.
- After a batch, fill in **Result** (date, commit, driver, one or two lines, log path)
  and move finished items to [Done](#done).
- Record the driver as the operator version and the Vulkan-reported one
  (for example 595.91.07 / 595.91.7.0).

Guest workloads come from [shadps4-open-test][open-test] at `eb1f2fe` unless an item
says otherwise. Runs use Vulkan core and synchronization validation. The known
fingerprints are `VUID-VkPipelineShaderStageCreateInfo-pNext-02755` and
`SYNC-HAZARD-WRITE-AFTER-PRESENT`; anything else counts as new.
The Gate B harness (`build-e3.sh`, `run-e3-matrix.sh`) and P1-A harness
(`run-graceful.sh`) are in the M75n experiment directories listed in
[PR #54][pr54]; adapt their paths for the NVIDIA host.

## Queue

### NV-1: sparse binding stays on the graphics queue

- **Why:** PR #54 picks a separate sparse queue family when the graphics family lacks
  sparse binding (`vk_instance.cpp`, `SubmitPendingArenaBinds`). This also changes the
  default, opt-out path. On M75n it was covered only with stock RADV (family 0 + 2) and
  with `radv_legacy_sparse_binding=true` (family 0). On NVIDIA the graphics family
  has sparse binding, so nothing should change.
- **Build:** the split queue-family commit `6017e23`, then PR #54 head (`c364436` or
  later). Baseline `66da8f1`. RelWithDebInfo.
- **Run:** with `SHADPS4_UMA_SHARED_BACKING` unset, and again with `=1`:
  `buffer_compute_roundtrip`, `map_unmap_reuse`, `uma_churn`. Grep the log for
  `Using sparse queue family`.
- **Expected:** no `Using sparse queue family` line (one queue family); no
  `VUID-vkQueueBindSparse-queuetype`; results and validation fingerprints identical
  to the baseline; no assert from a non-success `bindSparse`.
- **Status:** queued
- **Result:** —

### NV-2: host-pointer import after the dedicated-allocation fix

- **Why:** the plan-B host-pointer path can chain `VkMemoryDedicatedAllocateInfo`
  with a buffer, which VUID-VkMemoryAllocateInfo-pNext-02806 forbids
  ([review][r-dedicated]). The refactored host-pointer path has only run on lavapipe.
- **Build:** PR #54 head (`c364436` or later). The fix is `05af770`: the chunk fails when
  `requiresDedicatedAllocation` is set, and the `pNext` is dropped. RelWithDebInfo.
- **Run:** `SHADPS4_UMA_SHARED_BACKING=1`, readbacks Precise and Disabled:
  `buffer_compute_roundtrip`, `buffer_cpu_rewrite`, `buffer_pingpong`,
  `physical_alias`, `map_unmap_reuse`, `uma_churn`, `dma_order`,
  `dma_mixed_endpoints`, `fallback_in_shared`. Note from the log whether
  `requiresDedicatedAllocation` is true on this driver.
- **Expected:** no VUID-02806 or other new fingerprints. Either real host-pointer
  imports with the same results as the lavapipe host-pointer column, or (if dedicated
  is required) every request falls back to the mirror and still passes.
- **Status:** queued (the fix landed in `05af770`)
- **Result:** —

### NV-3: normal shutdown with host-pointer chunks

- **Why:** P1-A (explicit release on SDL quit) was verified only with DMA-BUF on M75n.
- **Build:** same as NV-2.
- **Run:** P1-A IPC `STOP` scenarios: `buffer_compute_roundtrip` (1 × 16 MiB),
  `map_unmap_budget` (4 × 16 MiB plus a mirrored fifth), and the
  `map_unmap_inflight` witness, 3 runs each.
- **Expected:** exit 0 without a forced signal; the log shows idle, the live chunk
  summary, then release; no lifetime errors or device loss.
- **Status:** queued, run after NV-2
- **Result:** —

### NV-4: pinning and PCIe cost of whole chunks

- **Why:** plan-B pins whole chunks and every GPU access goes over PCIe
  (kaaburgh/shadPS4#10). Nothing has been measured.
- **Build:** same as NV-2, plus mirror-only on the same commit.
- **Run:** focused remap budget test and `uma_churn`, shared vs mirror, 3 runs each.
  Record pinned memory (`VmLck`/`VmPin`, `nvidia-smi`), RSS and wall time.
- **Expected:** no fixed threshold yet; this produces the first numbers.
- **Status:** queued, run after NV-2; low priority
- **Result:** —

### NV-5: sparse binding of imported host memory (GPUVM model)

- **Why:** the [architecture review][r-arch] proposes binding imported guest backing into
  the sparse guest-VA arenas, so host GPU page tables mirror the guest mapping. Whether
  NVIDIA accepts host-pointer memory in `vkQueueBindSparse` decides if plan B can follow
  this model. lavapipe reports support but does not alias the pages, so it cannot answer.
- **Build:** `cc -O1 -g -o uma_gpuvm_probe tools/uma_gpuvm_probe/uma_gpuvm_probe.c -lvulkan`
  (standalone, no emulator build).
- **Run:** `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./uma_gpuvm_probe --backend host --mib 64`,
  then the same with `--mib 256`.
- **Expected:** record `Q`, S1–S3 and every `bind:` timing, plus the header values `block`,
  `residencyNonResidentStrict`, `minImportedHostPointerAlignment` and
  `sparseResidencyAliased`. PASS means the model works on NVIDIA. If the control passes but host-pointer fails, plan B keeps per-chunk buffers. Bind
  time per 64 KiB block is the cost of each guest map and unmap.
- **Status:** queued
- **Result:** —

### NV-6: PCIe bandwidth of imported memory vs VRAM

- **Why:** on a discrete GPU, zero-copy for every resource moves GPU traffic onto PCIe.
  The review recommends placement by access pattern (shared for CPU-written and readback
  data, VRAM for GPU-hot data). This gives the first numbers for that policy.
- **Build:** same probe as NV-5.
- **Run:** `./uma_gpuvm_probe --backend host --mib 8 --bw-mib 256`, 3 runs.
- **Expected:** `B1` lines with local→local, shared→local and local→shared GB/s.
  No threshold yet.
- **Status:** queued
- **Result:** —

### NV-7: the same pages imported twice at different offsets

- **Why:** the [granularity note][r-gran] shows that a 64 KiB block can be bound only if
  `VA ≡ PA (mod 64 KiB)` for the import it uses. A mapping with another phase needs a second
  import of the same pages, shifted by 16, 32 or 48 KiB (counterexamples CE1 and CE3).
  Vulkan defines aliasing per memory object, so nothing guarantees that two imports of the
  same pages stay consistent, or that the driver accepts the second import at all.
- **Build:** needs a probe mode that does not exist yet: import the backing a second time at
  `base + 16 KiB`, bind one block of each import into the arena, write through one and read
  through the other with a barrier between.
- **Run:** to be written with the probe mode. Also on M75n with dma-buf.
- **Expected:** import accepted or refused; bytes consistent in both directions; whether
  `minImportedHostPointerAlignment` permits a 16 KiB shift.
- **Status:** blocked on the probe mode, and only worth running if NV-5 passes
- **Result:** —

### NV-8: host-pointer import of a guest VA range

- **Why:** a block whose pages lie on two physical runs (CE2, CE4 of the
  [granularity note][r-gran]) has no contiguous import. On the host-pointer path the guest
  VA range itself is such a "stitched" view, because AddressSpace maps the memfd there in
  16 KiB pieces. This asks whether NVIDIA imports a range made of several mappings of a
  shared memfd, and what a live import sees after one piece is remapped.
- **Build:** needs a probe mode that does not exist yet: map permuted 16 KiB memfd pieces
  into one VA range (as test `E1` does for an exported buffer), import that range, bind it
  sparsely, then remap one piece.
- **Run:** to be written with the probe mode.
- **Expected:** import accepted or refused; GPU sees the permuted pages; after the remap,
  whether the import still points at the old page (expected, since imports pin pages).
- **Status:** blocked on the probe mode, and only worth running if NV-5 passes
- **Result:** —

## Done

None yet.

[open-test]: https://github.com/kaaburgh/shadps4-open-test/tree/eb1f2fefb1afc01baa2e2fa15265c81135b1c8e9
[pr54]: https://github.com/kaaburgh/shadPS4/pull/54
[r-arch]: https://github.com/kaaburgh/shadPS4/pull/54
[r-gran]: https://github.com/kaaburgh/shadPS4/pull/57
[r-dedicated]: https://github.com/kaaburgh/shadPS4/pull/54#discussion_r4578387976
