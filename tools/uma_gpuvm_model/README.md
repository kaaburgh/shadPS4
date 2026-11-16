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

32 tests, about 10 seconds, deterministic.

- `ExhaustiveConditions`: the closed-form block rule equals a brute-force search over every
  legal bind, for every map of a small universe.
- `Counterexamples`: CE1 to CE6 of the note, one test each.
- `TranslationTable`: a 16 KiB table is exact where a block-granular one is not.
- `Report`: the summary and the request classes.
- `Census`: the replay of an event stream, one property per test (repeated requests, sizes
  that are not page multiples, aliases, unbacked mappings, the peak, incomplete captures).
- `RealCapture`: the replay and `check_open_test.py` on `testdata/`, a capture of a real run,
  and on damaged copies of it.

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

- `capture`: `complete`, and if not, the `problems` found by `analyze.integrity` of
  `tools/uma_e0`. An incomplete capture is replayed up to its last trusted record
  (`valid_events`); a warning goes to stderr. Its counts are lower bounds.
- `block_bytes` and `block_bytes_source`: `argument`, `recorded` or `default`.
- `buffer_requests`: every BufferCache request, classified against the memory map in effect
  when it was made. Per class: `requests`, `bytes` (summed over requests), `unique_ranges`,
  `unique_bytes` (the union of the ranges) and `paths` (what shadPS4 did with the request:
  `mirror`, `stream`, `image_staging`, `shared`). `buffer_requests_total` is the same over all
  classes. A range that is requested every frame adds to `requests` and `bytes` only.
  - `contiguous`: one physical run inside one import chunk. A per-chunk buffer serves it.
  - `contiguous_cross_chunk`: one physical run across chunks.
  - `discontiguous_phase0`: needs the sparse VA buffer; block-aligned imports are enough.
  - `discontiguous_regular`: needs the sparse VA buffer with shifted imports.
  - `discontiguous_irregular`: no contiguous import can serve it.
  - `unbacked`, `unmapped`: touches memory with no importable backing, or no mapping.
- `at_peak_backed` (the moment most backed pages were mapped) and `final`:
  - `backed_guest_pages`, `unbacked_guest_pages`, `guest_pages_by_type`.
  - `aliased_physical_pages`: physical pages mapped at more than one VA, and
    `aliased_physical_pages_other_phase`: those whose VAs differ in their offset in a block.
  - `blocks`: `regular_full`, `regular_partial`, `irregular`, `mixed_backing` (backed and
    unbacked pages in one block) and `unbacked`. `regular_phase_bytes` counts regular blocks
    per phase. `blocks_holding_type` repeats this for direct, flexible and pooled memory.
  - `policies`: per policy the shared and fallback blocks, memory objects, mixed-alias pages
    and cross-object alias pages. Unbacked blocks (code, stack, file mappings, which have no
    Piece events) are not fallback blocks.
- `final_is_end_of_run`: false for an incomplete capture.

Class names at a block size other than the recorded one describe the recorded guest map under
that block size. They say nothing about a driver that reports it.

## Checking the census against real runs

```sh
python3 tools/uma_gpuvm_model/check_open_test.py RUN_DIR [RUN_DIR ...]
```

`RUN_DIR` is a run directory of `tools/uma_e0/run_synthetic.py` for a workload of
kaaburgh/shadps4-open-test. The script compares the census with `open_test_expectations.py`
(written from the guest sources before the first capture), with the map and munmap calls in the
emulator log, and with the path each request took. How to record the runs, and the results:
[docs/research/uma-e0-census-validation.md](../../docs/research/uma-e0-census-validation.md).
