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
- **Build:** PR #54 head (`d3d8796` or later; if the queue change is split into its own
  commit, test that commit). Baseline `66da8f1`. RelWithDebInfo.
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
- **Build:** the commit that fixes the review comment (fail the chunk when
  `requiresDedicatedAllocation`, drop the `pNext`). RelWithDebInfo.
- **Run:** `SHADPS4_UMA_SHARED_BACKING=1`, readbacks Precise and Disabled:
  `buffer_compute_roundtrip`, `buffer_cpu_rewrite`, `buffer_pingpong`,
  `physical_alias`, `map_unmap_reuse`, `uma_churn`, `dma_order`,
  `dma_mixed_endpoints`, `fallback_in_shared`. Note from the log whether
  `requiresDedicatedAllocation` is true on this driver.
- **Expected:** no VUID-02806 or other new fingerprints. Either real host-pointer
  imports with the same results as the lavapipe host-pointer column, or (if dedicated
  is required) every request falls back to the mirror and still passes.
- **Status:** blocked on the VUID-02806 fix
- **Result:** —

### NV-3: normal shutdown with host-pointer chunks

- **Why:** P1-A (explicit release on SDL quit) was verified only with DMA-BUF on M75n.
- **Build:** same as NV-2.
- **Run:** P1-A IPC `STOP` scenarios: `buffer_compute_roundtrip` (1 × 16 MiB),
  `map_unmap_budget` (4 × 16 MiB plus a mirrored fifth), and the
  `map_unmap_inflight` witness, 3 runs each.
- **Expected:** exit 0 without a forced signal; the log shows idle, the live chunk
  summary, then release; no lifetime errors or device loss.
- **Status:** blocked on NV-2
- **Result:** —

### NV-4: pinning and PCIe cost of whole chunks

- **Why:** plan-B pins whole chunks and every GPU access goes over PCIe
  (kaaburgh/shadPS4#10). Nothing has been measured.
- **Build:** same as NV-2, plus mirror-only on the same commit.
- **Run:** focused remap budget test and `uma_churn`, shared vs mirror, 3 runs each.
  Record pinned memory (`VmLck`/`VmPin`, `nvidia-smi`), RSS and wall time.
- **Expected:** no fixed threshold yet; this produces the first numbers.
- **Status:** blocked on NV-2; low priority
- **Result:** —

## Done

None yet.

[open-test]: https://github.com/kaaburgh/shadps4-open-test/tree/eb1f2fefb1afc01baa2e2fa15265c81135b1c8e9
[pr54]: https://github.com/kaaburgh/shadPS4/pull/54
[r-dedicated]: https://github.com/kaaburgh/shadPS4/pull/54#discussion_r4578387976
