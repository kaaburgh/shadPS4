<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# UMA E0 capture to census: check on real guest workloads

Status: done on 2026-11-16, on lavapipe in a Linux container. Nothing here was run on RADV or
NVIDIA, and nothing here is a statement about them.

The question: `tools/uma_gpuvm_model/gpuvm_model.py census` had only seen event streams written
by hand. Does the chain from the E0 recorder inside shadPS4 to the census give correct results
on real runs of PS4 guest code?

## 1. Result

The classification logic of the census was correct on every real capture. The chain around it
was not, in four places:

1. **The recorder never closed the capture of a guest that exits normally.** 12 of 12 first
   captures were unclosed, 11 of them ended inside a record, and both analyzers raised on them.
2. **On a full disk the recorder wrote records at the wrong offset** (6 of 8 runs) and replaced
   `metadata.json` by an empty file. The shifted records parsed as Map and Unmap events.
3. **Three aggregates were misleading**: code, stack and file mappings counted as "fallback"
   blocks of every policy; requests had no measure of unique memory; nothing told an incomplete
   capture from a complete one.
4. **One of my predictions was wrong** (SIGTERM, section 6), and one mapping was not predicted.

All four are fixed or corrected in this PR. After the fixes, 21 runs of 8 workloads pass 229
checks against references that do not come from the census (section 4).

## 2. What was run

| | |
| --- | --- |
| shadPS4 | this branch at `c5484fc` (recorded as `source_sha` in every capture), RelWithDebInfo, clang 19.1.1 |
| Guest workloads | kaaburgh/shadps4-open-test at `eb1f2fe`: `buffer_compute_roundtrip`, `physical_alias`, `map_unmap_reuse`, `uma_churn`, `fallback_in_shared`, `dma_alias_order`, `buffer_granularity_matrix`, `buffer_page_false_sharing` |
| Vulkan | lavapipe, Mesa 25.2.8 (`llvmpipe (LLVM 20.1.2, 256 bits)`), under a private Xvfb |
| Host | Ubuntu 24.04.5 container, x86-64, 4 cores, kernel 6.18 |
| Modes | each workload once on the mirror path and once with `SHADPS4_UMA_SHARED_BACKING=1` (lavapipe: host-pointer chunks of 256 MiB) |

Readbacks were `precise` in every run. A run takes 1 to 3 seconds.

## 3. How to reproduce

Packages (Ubuntu 24.04):

```sh
apt-get install -y --no-install-recommends clang-19 lld-19 clang-tools-19 libstdc++-14-dev \
  cmake ninja-build pkg-config glslc xvfb mesa-vulkan-drivers libvulkan-dev \
  libasound2-dev libpulse-dev libopenal-dev libssl-dev zlib1g-dev libedit-dev libudev-dev \
  libevdev-dev libsdl2-dev libjack-dev libsndio-dev libpng-dev libxtst-dev libxss-dev \
  libxkbcommon-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxext-dev libx11-dev \
  libwayland-dev wayland-protocols libdecor-0-dev libdrm-dev libgbm-dev libegl1-mesa-dev \
  libgl1-mesa-dev libdbus-1-dev libibus-1.0-dev libpipewire-0.3-dev libusb-1.0-0-dev
```

shadPS4:

```sh
git submodule update --init --recursive --depth 1
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
  -DCMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=/usr/bin/clang-scan-deps-19 -DENABLE_UPDATER=OFF
ninja -C build shadps4
```

Two steps failed in the container because its network policy blocks two hosts. Neither is
needed on a machine with open network access:

- `externals/aacdec/fdk-aac` comes from `android.googlesource.com`. The pinned commit is also at
  `https://github.com/mstorsjo/fdk-aac.git`: `git fetch --depth 1 <that URL> <pinned commit>`
  inside the submodule directory, then `git checkout --detach FETCH_HEAD`.
- CMake downloads fmt as a GitHub archive. Clone `https://github.com/fmtlib/fmt.git` at the tag
  CMake asks for and add `-DFETCHCONTENT_SOURCE_DIR_FMT=<clone>`.

Guest workloads (needs the OpenOrbis toolchain, which the bootstrap script downloads):

```sh
git clone https://github.com/kaaburgh/shadps4-open-test && cd shadps4-open-test
git checkout eb1f2fe
bash scripts/bootstrap-deps.sh
for t in buffer_compute_roundtrip physical_alias map_unmap_reuse uma_churn fallback_in_shared \
         dma_alias_order buffer_granularity_matrix buffer_page_false_sharing; do
  bash scripts/build-test.sh $t
done
```

Record and check, from the shadPS4 tree:

```sh
tools/uma_e0/run_open_test_matrix.sh build/shadps4 ../shadps4-open-test/out RUNS
python3 tools/uma_gpuvm_model/check_open_test.py RUNS/*/
python3 -m unittest tools/uma_e0/test_analyze.py tools/uma_gpuvm_model/test_gpuvm_model.py
```

`run_open_test_matrix.sh` records 16 workload runs and 5 runs that end the capture in other ways.
The last of those mounts a 128 KiB tmpfs and therefore needs root; without root it is skipped.
One run by hand:

```sh
python3 tools/uma_e0/run_synthetic.py --shadps4 build/shadps4 \
  --elf ../shadps4-open-test/out/uma_churn/uma_churn.elf --output RUNS/one --xvfb --lavapipe
python3 tools/uma_gpuvm_model/gpuvm_model.py census RUNS/one/capture --block 65536
```

The recorder is Linux only (`uma_census.cpp` returns early elsewhere), so there is no capture on
macOS. `run_synthetic.py` is not tied to a game: it takes any guest ELF and ends the run on a
log marker or after a time. The existing `run_capture.py` stays as the Bloodborne scenario.

Expected output of the checker: `0 failed checks`, exit status 0. The guest results differ from
that and are listed in section 5.1.

## 4. What the census was compared with

Three references, none of which is computed by the census:

- **Predictions.** `tools/uma_gpuvm_model/open_test_expectations.py` was written from the guest
  sources and committed (`fc155bb`) before the first capture existed: the direct mappings each
  test makes, aliased pages, block classes, request counts, summed and unique bytes, request
  classes. Two corrections were made afterwards and are marked in the file (section 6).
- **The emulator log.** `run.log` holds the `sceKernelMapNamedDirectMemory` and
  `sceKernelMunmap` lines of the kernel HLE. `check_open_test.py` replays them into its own page
  table and compares it with a table built from Map, Piece and Unmap events, after every call
  and at the end. This checks Map, Piece, Unmap and UnmapCall events and their physical
  addresses without using `gpuvm_model.py`.
- **The path the emulator took.** Every Buffer event records what `BufferCache` did with the
  request. With shared backing, shadPS4 serves a range from an imported chunk only if it is one
  physical run inside one chunk. So at the recorded block and the real chunk size, class
  `contiguous` must coincide with path `shared`, request by request.

The checker itself is tested on a real capture: changing one physical address, one request size
or one path field in `testdata/` makes the corresponding check fail (`RealCapture` tests).

## 5. Results

### 5.1 Runs

Final matrix, recorder with the fixes of this PR. "Events" is the number of records in
`events.bin`.

| Run | Guest result | Events | Capture complete | Checks passed / failed |
| --- | --- | ---: | --- | --- |
| buffer_compute_roundtrip, mirror | PASS | 288 | yes | 13 / 0 |
| buffer_compute_roundtrip, shared | PASS | 207 | yes | 14 / 0 |
| physical_alias, mirror | **FAIL** | 898 | yes | 15 / 0 |
| physical_alias, shared | PASS | 555 | yes | 16 / 0 |
| map_unmap_reuse, mirror | PASS | 872 | yes | 14 / 0 |
| map_unmap_reuse, shared | PASS | 466 | yes | 15 / 0 |
| uma_churn, mirror | PASS | 46419 | yes | 14 / 0 |
| uma_churn, shared | PASS | 1171 | yes | 15 / 0 |
| fallback_in_shared, mirror | PASS | 304 | yes | 16 / 0 |
| fallback_in_shared, shared | PASS | 250 | yes | 17 / 0 |
| dma_alias_order, mirror | **FAIL** | 547 | yes | 12 / 0 |
| dma_alias_order, shared | **FAIL** | 427 | yes | 13 / 0 |
| buffer_granularity_matrix, mirror | PASS | 5864 | yes | 9 / 0 |
| buffer_granularity_matrix, shared | PASS | 2213 | yes | 10 / 0 |
| buffer_page_false_sharing, mirror | PASS | 580 | yes | 9 / 0 |
| buffer_page_false_sharing, shared | PASS | 363 | yes | 10 / 0 |
| uma_churn, IPC STOP after 0.6 s | cut short | 1225 | yes | 3 / 0 |
| uma_churn, SIGTERM after 0.6 s | cut short | 1227 | yes | 3 / 0 |
| uma_churn, SIGKILL after 0.6 s | cut short | 1223 | no, as it must be | 4 / 0 |
| uma_churn, budget of 208000 bytes | PASS | 2000 | no, as it must be | 3 / 0 |
| uma_churn, 128 KiB file system | PASS | 1220 | no, as it must be | 4 / 0 |

The three guest FAIL results are the guest's own verdict on the emulator (stale data through a
physical alias on the mirror path, and the DMA alias cases). They are the known alias issues of
PR #54 and were not investigated here. A guest FAIL does not affect the capture or the checks:
the memory map and the requests are the same.

The last two workloads have no predictions; they get the log, block and path checks only.

### 5.2 Predictions against the census

Block size 64 KiB unless stated. Every row matched.

| Workload | Predicted before the capture | Census |
| --- | --- | --- |
| buffer_compute_roundtrip | one direct mapping of 2 MiB, no alias, direct blocks regular with phase 0, all requests `contiguous` | same; 33 direct blocks, 2 requests |
| physical_alias | mappings of 2 MiB, 256 KiB, 256 KiB; 16 aliased physical pages, same phase; no irregular block; 14 requests | 16 aliased pages, 0 of them with another phase, 41 direct blocks all `regular_full`, 14 `contiguous` requests |
| map_unmap_reuse | 256 KiB mapped three times at one VA; no alias; 18 requests | three map calls and three munmap calls in the same order as the log; 18 `contiguous` requests |
| uma_churn | pool of 4 MiB mapped four times; 112 requests | four maps; 112 `contiguous` requests |
| fallback_in_shared | 4 `contiguous`, 1 `contiguous_cross_chunk` (range straddles 256 MiB), 1 discontiguous; at 16 KiB `discontiguous_phase0`; with one chunk over all memory the straddle range becomes `contiguous` | exactly that; the discontiguous range is `discontiguous_irregular` at 64 KiB (VA `0x20020c000`, one of the two classes allowed for that VA) |
| dma_alias_order | 13 direct map calls, 4 aliased pages, at least one irregular direct block at 64 KiB and none at 16 KiB | 13 calls, 4 aliased pages (all 4 with another phase), 3 irregular direct blocks at 64 KiB, 0 at 16 KiB |

The emulator's own decision, shared-backing runs, recorded block 16 KiB and real chunk of
256 MiB:

| Run | `contiguous` requests, path | Other requests, path |
| --- | --- | --- |
| buffer_compute_roundtrip | 2, all `shared` | none |
| physical_alias | 14, all `shared` | none |
| map_unmap_reuse | 18, all `shared` | none |
| uma_churn | 112, all `shared` | none |
| fallback_in_shared | 4, all `shared` | `contiguous_cross_chunk` 1 and `discontiguous_phase0` 1, both `mirror` |
| dma_alias_order | 19, all `shared` | `discontiguous_phase0` 1, `mirror` |
| buffer_granularity_matrix | 270, all `shared` | none |
| buffer_page_false_sharing | 20, all `shared` | none |

No request was classified `contiguous` and served by the mirror, and none the other way round.

### 5.3 Requests, bytes and unique memory

| Workload | Requests | Bytes summed over requests | Unique ranges | Bytes of their union |
| --- | ---: | ---: | ---: | ---: |
| physical_alias | 14 | 917504 | 11 | 720896 |
| map_unmap_reuse | 18 | 589824 | 4 | 131072 |
| uma_churn | 112 | 135135616 | 5 | 4251660 |
| fallback_in_shared | 6 | 196608 | 6 | 196608 |
| buffer_granularity_matrix | 270 | 3371772 | 60 | 796540 |
| buffer_page_false_sharing | 20 | 16400 | 16 | 12304 |

The first four rows were predicted exactly. `uma_churn` is the case the question was about:
129 MiB of "request bytes" are 4 MiB of memory requested 32 times. The census printed only the
first two columns before this PR. It now prints all four per class and in total, and a figure
in bytes must be read with its unique counterpart next to it.

Request counts also depend on the emulator mode, not only on the guest. `dma_alias_order` makes
15 Buffer events on the mirror path and 20 with shared backing, and its discontiguous request
exists only in the second. DMA fast paths and image sources emit no Buffer event. Compare
counts between captures only when they were recorded in the same mode.

### 5.4 Complete and incomplete captures

Before the fixes (first matrix, 12 runs, each guest exits normally):

| | Count |
| --- | ---: |
| `clean_shutdown` false, no End event | 12 of 12 |
| `events.bin` ends inside a record | 11 of 12 |
| `metadata.json` counts fewer events than the file holds | 12 of 12 (for example 187 against 856) |
| `analyze.py` and `gpuvm_model.py census` raise `truncated event record` | 11 of 12 |
| Last map events missing (`uma_churn`: 132 direct pages still mapped, the log ends with 4) | 2 of 12 |

After the fixes every such capture is closed by the guest's `exit`, and the checker requires
it. The other ways to end a capture:

| How the run ended | Capture | Reported by `analyze.integrity` |
| --- | --- | --- |
| Guest exits | complete | nothing |
| IPC STOP, mid-run | complete | nothing |
| SIGTERM, mid-run | complete | nothing |
| SIGKILL, mid-run | whole records, readable | not shut down cleanly; metadata counts 54 events, file holds 1223; no End event |
| Byte budget reached | 2000 records, then counted drops | 44381 events dropped; no End event |
| Disk full | 1220 whole records, older metadata kept | not shut down cleanly; metadata and file disagree; no End event |

The census replays an incomplete capture up to its last trusted record, sets
`capture.complete` and `final_is_end_of_run` to false, lists the problems and warns on stderr.
It no longer presents such a result as the state at the end of a run.

## 6. Defects found, and what was done

**Recorder: capture not closed on guest exit** (`src/core/libraries/kernel/process.cpp`). The
guest's `exit` ends in a trap, which skips `atexit`, so `UmaCensus::Shutdown` never ran. `exit`
now closes the capture first and records `guest_exit_status`. No effect without a capture.

**Recorder: shifted records and lost metadata on write errors** (`src/common/uma_census.cpp`).
Events went through a 4096 byte stdio buffer, which is not a multiple of the 104 byte record.
On a 128 KiB file system the old writer lost a buffer and appended later ones in 6 of 8 runs.
From record 1221 on, every record was read at the wrong offset: the 39 records after it parsed
mostly as Map and Unmap events, with timestamps in the sequence field. `metadata.json` was
replaced by an empty file in 8 of 8 runs. Now events are written with `write`, whole records
only. The first failed or short write cuts the file back to the last whole record and ends it,
later events are counted as dropped, the emulator log says
`UMA E0 capture stopped after N events`, and `metadata.json` is replaced only by a file that
was written in full. A side effect: `written_events` is what the file holds, and a killed
process leaves whole records.

**Analyzer: no notion of an incomplete capture.** `analyze.records` raised on a partial last
record, and nothing checked the metadata against the file. `analyze.integrity()` now reports:
unreadable metadata, wrong schema, a file that ends inside a record, a record that does not
continue the sequence (it and what follows is not trusted), unclean shutdown, a write error,
counted drops, a metadata count that differs from the file, events reserved but neither written
nor counted, a missing End event of the recorder, and events after it. Timeline observers also
emit End events; those carry a context and are not taken for the end of the capture.
`analyze.analyze()` keeps rejecting a damaged capture, as its test requires.

**Census: unbacked mappings counted as fallback.** Code, stack and file mappings have Map events
without Piece events. In `buffer_compute_roundtrip` they are 11327 of 11472 pages. At 16 KiB
the census reported 11327 fallback blocks for every policy, which read as "99 % of the address
space cannot be shared". These blocks are now `unbacked` and outside the policy counts; a block that
mixes backed and unbacked pages is `mixed_backing`.

**Census: other corrections.** Unique ranges and bytes per request class (section 5.3). Aliased
physical pages, split by phase. Block classes per VMA type. The peak is the moment of most
backed pages, not of most mapped pages. A range covers every page it touches, also when its
size is not a multiple of 16 KiB.

**Predictions that were wrong or missing.**

- I predicted that a capture stopped with SIGTERM cannot be clean, because shadPS4 installs no
  handler. Wrong: SDL turns SIGTERM into a quit event, and the capture is closed. The entry is
  corrected and marked in `open_test_expectations.py`.
- I did not predict a 64 KiB direct mapping named `SceGnmDriver`, which OpenGNM makes before the
  test code runs. It takes physical address 0, so the arena of every test starts at 2 MiB, not
  at 0. The checker leaves this one mapping out of the comparison of mapping sizes.

## 7. Observations that were not predicted

- **Flexible memory is not phase-aligned in these runs.** In `uma_churn` the guest's heap
  (flexible memory, 343 pages at peak) forms 83 full 64 KiB blocks with phase 16 KiB and one
  irregular block. Under the model, block-aligned imports share 99 of 185 backed blocks,
  shifted imports 182. All direct blocks have phase 0. This is the map shadPS4 produced for
  this guest, read under a 64 KiB block; it supports checking recommendation 5 of the
  granularity note (choose phase-matched addresses) on real titles.
- A flexible page directly after the arena makes one block `irregular` in `fallback_in_shared`
  and `dma_alias_order`. Neighbouring mappings of other types decide block classes; a
  per-mapping view would miss this.
- Run-to-run variation: event counts of the same workload differ by a few records between runs
  (timeline and watch events). Map, Piece, Unmap and Buffer events did not vary.

## 8. What this does not show

- **No driver behaviour.** lavapipe reports a 4 KiB sparse block, which shadPS4 clamps to
  16 KiB, equal to the guest page. Every figure at 64 KiB is the model applied to the recorded
  guest map and requests. Whether RADV or NVIDIA accept the binds the model calls legal is the
  subject of the hardware probe and is untested.
- **No real title.** The workloads are small and regular by design. They exercise every request
  class except `discontiguous_regular`, `unbacked` and `unmapped`, which appear only in the
  hand-written streams of the unit tests.
- **The shared path on lavapipe uses host-pointer imports.** The dma-buf path of the M75n was not
  run.
- **Mid-run remaps.** The log comparison covers direct mappings and `sceKernelMunmap`. Pooled
  memory and `mprotect` do not occur in these workloads.
- **The Bloodborne captures on the M75n** were recorded with the old writer and were not looked
  at here. `run_capture.py` ends a run through IPC STOP, which closes the capture, but
  `analyze.integrity()` should be run on them before the census is.
