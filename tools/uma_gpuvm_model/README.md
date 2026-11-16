<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# UMA GPUVM granularity model

A CPU-only model of one question: which guest memory maps (16 KiB pages) can a Vulkan sparse
buffer indexed by guest VA represent, for a given sparse block size and kind of imported memory
object. It needs Python 3 and nothing else: no Vulkan, no GPU, no emulator build.

The reasoning and the conclusions are in
[docs/research/uma-gpuvm-granularity.md](../../docs/research/uma-gpuvm-granularity.md).

The model encodes the specification rules for `VkSparseMemoryBind` on buffers. It says nothing
about what a driver supports. Driver questions go to `tools/uma_gpuvm_probe`.

## Test

```sh
python3 -m unittest tools/uma_gpuvm_model/test_gpuvm_model.py
```

17 tests, about 3 seconds, deterministic.

- `ExhaustiveConditions`: the closed-form block rule equals a brute-force search over every
  legal bind, for every map of a small universe.
- `Counterexamples`: CE1 to CE6 of the note, one test each.
- `TranslationTable`: a 16 KiB table is exact where a block-granular one is not.
- `Report`: the summary and the census replay.

## Scenarios

```sh
python3 tools/uma_gpuvm_model/gpuvm_model.py scenarios            # 4, 16 and 64 KiB blocks
python3 tools/uma_gpuvm_model/gpuvm_model.py scenarios --block 65536 --strict
```

Each line shows `shared blocks / non-empty blocks` per import policy:

- `phase0`: imports start at block multiples (the chunk layout of PR #54).
- `multiphase`: imports may start at any guest page.
- `stitched`: additionally one list-built memory object per block that needs it.

## Classifying a real capture

```sh
python3 tools/uma_gpuvm_model/gpuvm_model.py census CAPTURE_DIR [--block 65536] [--chunk BYTES]
```

`CAPTURE_DIR` is a UMA E0 capture (`events.bin`, see `tools/uma_e0`). The replay needs Map,
Piece and Unmap events. Without `--block` it uses the block size recorded in the capture.

Output, as JSON:

- `buffer_requests` and `buffer_request_bytes`: every BufferCache request classified against the
  memory map in effect when it was made.
  - `contiguous`: one physical run inside one import chunk. A per-chunk buffer serves it.
  - `contiguous_cross_chunk`: one physical run across chunks.
  - `discontiguous_phase0`: needs the sparse VA buffer; block-aligned imports are enough.
  - `discontiguous_regular`: needs the sparse VA buffer with shifted imports.
  - `discontiguous_irregular`: no contiguous import can serve it.
  - `unbacked`, `unmapped`: touches memory with no importable backing, or no mapping.
- `at_peak_mapped` and `final`: block classes, phases of regular blocks, and per policy the
  shared and fallback block counts, memory objects, mixed-alias pages and cross-object alias
  pages.

The replay has been tested on synthetic event streams only.
