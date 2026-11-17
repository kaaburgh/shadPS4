<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# UMA E1C: WRITE_DATA ordering with shared backing

Write side of kaaburgh/shadPS4#11 for `WRITE_DATA`. Base: PR #54 at `c364436`. Fix: `8971952`;
oracle tool: `5e41afe`. Verified on lavapipe (host-pointer backend) and on RADV (M75n, dma-buf
backend), 2026-11-17.

## Cause

`Liverpool::ProcessGraphics` and `ProcessCompute` handled `WRITE_DATA` as a `std::memcpy` into
guest memory while parsing the packet. With shared backing a dispatch recorded earlier binds its
buffer from the shared chunk, which is guest memory, and reads it only when the command buffer
executes. That happens after the next submit (EOP, `SubmitDone`, or a CP wait), so the parse-time
write always overtook it:

```
dispatch A reads X    recorded, not submitted
WRITE_DATA X = new    memcpy now: guest memory holds new
dispatch B reads X    recorded
EOP                   submit: A and B both read new
```

The mirror is not affected: A uploaded X into the mirror while it was recorded, the memcpy marks
the page CPU-modified, and B uploads again. A shader write to X followed by a `WRITE_DATA` had the
mirror image of the problem: the CP's words were overwritten when the shader ran.

## Mechanism of the fix

`Rasterizer::CpWriteData` replaces the memcpy in both handlers and splits the write at
BufferCache blocks (16 KiB on lavapipe and RADV up to Mesa 26.1):

| Destination block | Write | Why |
|---|---|---|
| shared backing off | memcpy while parsing | unchanged behaviour |
| not in `shared_ranges` (mirror, stream, unused) | memcpy while parsing | recorded work holds its own copy or none; mirror semantics |
| in `shared_ranges`, 4-byte aligned | `vkCmdUpdateBuffer` into the chunk buffer | ordered by the command stream after earlier work and before later work |
| in `shared_ranges`, misaligned | wait for recorded work, then memcpy | `vkCmdUpdateBuffer` needs offset and size multiples of 4 |

`Runtime::UpdateBuffer` puts a full memory barrier before and after the update. Shaders also
reach shared blocks through the BDA page table, and those accesses have no tracked range, so
the range tracking of `AccessBuffer`/`IsBufferAccessed` alone would miss them. `vkCmdUpdateBuffer`
copies the data into the command buffer when recorded, so the packet's memory may be reused. A
`WRITE_DATA` carries at most 16381 dwords, under the 65536-byte limit, and a block piece is at most
one block. `texture_cache.InvalidateMemoryFromGPU` marks images at that address for a GPU-timeline
reupload, as `CopyBuffer` does.

Nothing on this path imports a chunk, demotes a block, changes the 64 MiB budget, the chunk
layout or the GPUVM arrangement. The only waits are on the CP thread between packets, never
while a draw or dispatch is being recorded and never inside a deferred operation (issue #7).

**Consequences of deferring the write**, handled in the same change:

- *CP reads of a pending write.* A GPU-timeline CP write is pending until its tick completes. The
  parse-time CP accesses (`COND_EXEC` predicate, `MEM_SEMAPHORE`, `WAIT_REG_MEM` memory polls,
  `DUMP_CONST_RAM`, `ZPASS_DONE` counters, indirect-buffer contents, `REWIND`, and a parse-time
  `WRITE_DATA` through another VA) call `Rasterizer::SyncCpAccess`, which compares physical
  ranges and waits (`Finish`, caller 8) only on overlap. Without it `COND_EXEC` after
  `WRITE_DATA` of its predicate picks the old branch (ablation `nosync`).
- *Liveness.* `rasterizer->Flush()` ran only on `SubmitDone`. A guest that polls a `WRITE_DATA`
  label without EOP would wait forever. `FlushPendingCpWrites` submits unsubmitted pending
  writes when the CP goes idle and when a queue yields in a wait (ablation `noflush`).

## Results on lavapipe (cloud, 2026-11-17)

Environment: Ubuntu 24.04, Linux 6.18, clang 19.1.1, Mesa lavapipe 25.2.8, validation layers
1.3.275, Xvfb. Shared backing on lavapipe is the host-pointer backend with 256 MiB chunks. Guest
tests: kaaburgh/shadps4-open-test `eb1f2fe` plus `write_data_interleave` at `8296f5e`. Runner:
`SHADPS4=BIN scripts/run-test-lavapipe.sh TEST --readbacks MODE [--shared-backing]`.

Binaries (sha256 prefix): `c364436` `6fc1de9550b2712b`; fix built from the working tree that was
committed as `8971952` (`a680a39482e4a60c`, used for the repeats and neighbours); `5e41afe`
committed build `c001d50ca18bc010`.

| Test | Config | c364436 | fix |
|---|---|---|---|
| write_data_order | mirror Precise | PASS | PASS 5/5 (+1 at 5e41afe) |
| write_data_order | shared Precise | FAIL `a_saw_later_write` (large_a, small_a) | PASS 5/5 (+1) |
| write_data_order | shared Disabled | FAIL `a_saw_later_write` | PASS 5/5 (+1) |
| write_data_interleave | mirror Precise | PASS | PASS 5/5 (+1) |
| write_data_interleave | shared Precise | FAIL `saw_later_write` (interleave, gpu_then_cp) | PASS 5/5 (+1) |
| write_data_interleave | shared Disabled | FAIL `saw_later_write` | PASS 5/5 (+1) |

Mirror Disabled of `write_data_order` fails `unwritten` before and after: its shader outputs need a
readback, as the scenario plan expects; the CP-written `x_values` pass.

**Neighbours**, mirror Precise, shared Precise and shared Disabled, baseline and fix, 22 tests:
identical results except `write_data_order` (and the new test). Unchanged known failures:
`cond_exec_gpu_predicate` shared (`branch_skipped`, read side of #11), `dma_alias_order` all
(`a_dispatch_after_fill`), `fallback_in_shared` shared Disabled, `physical_alias` mirror,
`rt_buffer_order` all, `image_buffer_alias`/`image_source_same_submit` mirror,
`tiled_rt_buffer_read` all. See `neighbours/`.

**Validation** (`VK_LAYER_KHRONOS_validation`, `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true`; the
probe canary V1 showed the layer reports a planted error). Both binaries, both tests, mirror and
shared Precise, 2 runs each. Every message in every run is one of two pre-existing ones:
`VUID-VkDeviceCreateInfo-pNext-pNext` at device creation, and `SYNC-HAZARD-WRITE-AFTER-WRITE` on
the BDA page table buffer (`vkCmdFillBuffer` at init, then `vkCmdCopyBuffer` of
`WriteBdaEntries` without a barrier). None names the shared chunk buffers or
`vkCmdUpdateBuffer`. The fix lowers the BDA hazard count in shared runs (10 to 2 in
`write_data_interleave`) because its barriers also order those copies; the hazard itself is
outside this change.

## Results on RADV (M75n, 2026-11-17)

Ryzen 3 3300U, Radeon Vega 6 (RADV RAVEN), Mesa 25.0.7, Debian 13 kernel 6.12, validation layers
1.4.309, headless weston. Both binaries built on the machine with the configuration of the earlier
PR #54 RADV builds (Release with `-O0` C++, clang-19, lld): baseline `c364436` sha256
`d22d29489c124d46…`, fix `2366205` (sources as `5e41afe`) sha256 `c87a180a2a9b759b…`. Every
shared-configuration log shows `backend=dma-buf`. Raw logs, commands, build logs and the README
with all hashes: `write-data-order-results/radv-m75n-20261117/`.

| Test | Config | c364436 | fix |
|---|---|---|---|
| write_data_order | mirror Precise | PASS 5/5 | PASS 5/5 |
| write_data_order | shared Precise | FAIL 5/5 `a_saw_later_write` (large_a, small_a) | PASS 5/5 |
| write_data_order | shared Disabled | FAIL 5/5 `a_saw_later_write` | PASS 5/5 |
| write_data_interleave | mirror Precise | PASS 5/5 | PASS 5/5 |
| write_data_interleave | shared Precise | FAIL 5/5 `saw_later_write` (interleave, gpu_then_cp) | PASS 5/5 |
| write_data_interleave | shared Disabled | FAIL 5/5 `saw_later_write` | PASS 5/5 |

An earlier PR #54 build of `c364436` gave the same baseline failure 3/3 (`baseline-c364436/step2/`).

**Neighbours**, once per configuration, 11 tests (`cond_exec_gpu_predicate`, `dma_order`,
`dma_alias_order`, `physical_alias`, `fallback_in_shared`, `map_unmap_reuse`,
`buffer_compute_roundtrip`, `buffer_pingpong`, `pm4_controls`, `ordered_async_handoff`,
`uma_churn`): the fix equals the baseline in all 33 runs. Known failures in both:
`cond_exec_gpu_predicate` shared, `dma_alias_order` (all mirror cases, non-contiguous shared
cases), `physical_alias` mirror, `fallback_in_shared` shared Disabled.

The fix's log line "first CP write on the GPU timeline" appears in every shared run of the two
`WRITE_DATA` tests and of `dma_order`, and never in mirror or baseline logs. "waits for the GPU"
appears in no log.

**Validation** (core plus synchronization; the layer printed that synchronization validation
was enabled, and the canary's planted VUID was reported): both tests, mirror and shared Precise,
2 runs each, both binaries. Every log has the same messages: only
`VUID-VkPipelineShaderStageCreateInfo-pNext-02755` (compute pipelines with a required subgroup
size on a stage not in `requiredSubgroupSizeStages`), no `SYNC-HAZARD`. No message differs
between baseline and fix. Synchronization validation tracks hazards per Vulkan object, so a
write through the chunk buffer and a read through another buffer over the same imported
memory would not be reported; the zero count is weak evidence on its own. The ordering evidence
is the guest tests and the ablations.

## Is the PASS real? Ablations and the host oracle

A guest test cannot tell an ordered write from one that waited for the GPU. The ablation binary
(`ablations/ablation-hooks.patch` on top of `5e41afe`, plus recorder fix `5b5190d` for whole E0
captures; neither is part of the branch) replaces parts of the fix under `WD_ABLATE`:

| Ablation | write_data_order | write_data_interleave | E0 oracle |
|---|---|---|---|
| `flush`: submit, then memcpy (no wait) | FAIL | FAIL `saw_later_write` | FAIL |
| `finish`: wait for the GPU, then memcpy | PASS | PASS | **FAIL** (31 parse-time writes right after a Finish) |
| `nosync`: no `SyncCpAccess` | PASS | FAIL `cond_after_cp` | PASS |
| `noflush`: no idle flush | PASS | FAIL `label_no_eop` | PASS |
| fix | PASS | PASS | PASS (28 of 30 timeline writes after an unexecuted dispatch) |

`tools/uma_e0/check_cp_write_order.py CAPTURE` counts GPU-timeline CP writes (Command 12)
recorded in the same submission as the last dispatch before them with no Finish in between, and
parse-time `WRITE_DATA` writes right after a Finish. On the fix, `write_data_order` gives 6 of 6
ordered writes and no Finish; `write_data_interleave` 28 of 30, the other two follow the one
expected wait of `cond_after_cp` (caller 8). `forbid-log.txt` of the new test also fails a run
that takes the misaligned-write wait.

## Remaining limits

- **Read side of #11** is unchanged: parse-time reads of GPU-written memory (sharps, user data,
  `COND_EXEC` on a shader-written predicate, `IsComputeMetaClear`) see guest memory before the
  recorded work ran. `cond_exec_gpu_predicate` still fails in shared mode.
- Parse-time **writes** of `DUMP_CONST_RAM`, `ZPASS_DONE` counters and `MEM_SEMAPHORE` into shared
  blocks still overtake recorded readers. They now wait for pending CP writes, so their order with
  `WRITE_DATA` holds. `CpWriteData` is the entry point to move them to the timeline next.
- A `WRITE_DATA` to a VA outside `shared_ranges` that aliases a shared block through another VA
  stays a parse-time write (issue #8 territory). `SyncCpAccess` orders it after pending CP writes,
  not after recorded shaders.
- A `WRITE_DATA` into the command buffer the CP is parsing is deferred if that memory is in a
  shared block, so the CP would read the old commands. No GPU work binds command memory in the
  tests; not handled.
- Images: the write marks images at the same base address `GpuDirty`; an image that starts
  elsewhere and covers the range is not invalidated (same rule as `CopyBuffer`).
- Each GPU-timeline `WRITE_DATA` costs two full memory barriers. Not measured on a title.
- The ablations and the E0 oracle ran on lavapipe only; on RADV the evidence is the guest tests
  and the absence of the wait line.
- A wait for a pending CP write (`SyncCpAccess`) is a `Finish` of all recorded work, not of the
  write's tick only.
- The completion of EOP/`RELEASE_MEM` labels is published after the GPU timeline, so it stays
  ordered after the write; unchanged.

## Artifacts

`write-data-order-results/radv-m75n-20261117/`: see its `README.txt`.

`write-data-order-results/lavapipe-20261117/`: `baseline-c364436/`, `fix-repeats/` (5 runs per
configuration, `summary.txt`), `final-5e41afe/`, `ablations/` (run logs, oracle JSON, capture
metadata, hook patch), `oracle/` (fix captures, 3 runs per test), `neighbours/` (matrix
scripts and summaries), `validation/`.
