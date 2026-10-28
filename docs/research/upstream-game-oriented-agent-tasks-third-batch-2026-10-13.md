<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Third batch — 10 game-oriented shadPS4 tasks, synthetic verification first

Prepared **2026-10-13**. **Planning only**: no code changes, builds, emulator runs, homebrew tests, or game compatibility improvements are claimed here.

**Upstream snapshot:** [shadps4-emu/shadPS4 \`23c41333822fe8eca4c609b43119180af63ae4ac\`](https://github.com/shadps4-emu/shadPS4/tree/23c41333822fe8eca4c609b43119180af63ae4ac). Examined current public upstream issues, open PR inventory (**79** at inspection) and target source paths. These ten are distinct from [batch #21](https://github.com/kaaburgh/shadPS4/pull/21) and [batch #22](https://github.com/kaaburgh/shadPS4/pull/22).

**Selection/ownership:** Prefer recognizable titles and discrete behavior over generic renderer rewrites. The game reports motivate the work; **do not infer a proven causal connection** without executing that game. Open upstream PRs were screened; notably omit Diablo III GPU clock ([#4981](https://github.com/shadps4-emu/shadPS4/issues/4981)) despite a confirmed suspicious \`GetGpuClock64\` implementation because [PR #5190](https://github.com/shadps4-emu/shadPS4/pull/5190) changes the same \`pm4_cmds.h\`. Also omit Vulkan pipeline cache cross-driver fix [#4878](https://github.com/shadps4-emu/shadPS4/issues/4878) while [#5246](https://github.com/shadps4-emu/shadPS4/pull/5246) edits \`vk_pipeline_cache.cpp\`; avoid voice changes during [#5149](https://github.com/shadps4-emu/shadPS4/pull/5149), save-data during [#5226](https://github.com/shadps4-emu/shadPS4/pull/5226), and shader/PM4 work in other active PRs. The exclusions are conservative. All negative overlap checks are public-only and should be repeated just before implementation.

## Evidence terminology and test contract

- **Observed** = source-specific issue seen on the inspected upstream tree. **Reported** = user issue, possibly closed/stale. **Hypothesis** = game symptom may be caused by that code; cannot be proven here.
- Work in small independent **draft PRs in kaaburgh/shadPS4 only**. When guest API integration matters, add a separate linked **draft PR in kaaburgh/shadps4-open-test**, using only OpenOrbis/OpenGNM and freely generated fixtures. **Never open agent PRs in upstream.**
- Red-before/green-after oracle; run host unit tests/ASan/UBSan and homebrew in shadPS4 under Mesa lavapipe as appropriate. Record SHAs, commands, timeouts, PASS/FAIL/UNVERIFIED and logs. Do not count clean compilation or a mere guest boot as a pass. Do not reuse Sony SDK, proprietary firmware/modules, game dumps/assets or copied reverse-engineered binary material.
- If a task hinges on undocumented ABI, game behavior, another repository's implementation, or a hardware feature not present in the test environment, gate it explicitly. **Do not invent return codes, “fix” a game's behavior by guesswork, or claim compatibility improvements from synthetic tests.**

## Ranked candidates

### 1. [P0 | game report + observed unsafe clock assumption] Bloodborne: nonmonotonic CPU TSC must not stall audio

- [Bloodborne-backed Linux audio report #5243](https://github.com/shadps4-emu/shadPS4/issues/5243) (closed; root-cause hypothesis is detailed and reproducible on the reporter's host). Current [\`src/common/native_clock.cpp\`](https://github.com/shadps4-emu/shadPS4/blob/main/src/common/native_clock.cpp) still derives uptime from \`FencedRDTSC()\`; [\`openal_audio_out.cpp\`](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/audio/openal_audio_out.cpp) performs audio timing/sleep.
- Task: isolate negative/large host-clock discontinuities, ensure monotonic guest time or appropriate fallback, and bound blocking audio sleeps without changing the normal synchronized-TSC fast path. Do not conflate this with batch #22's **SDL queue-clearing** issue.
- **Without a game:** deterministic injected clock trace (backward and forward jumps, thread migration proxy) plus synthesized audio timing, asserting bounded block duration and nondecreasing guest clock; optionally a generated OpenOrbis audio-output guest. Actual hardware CPU skew is a separate optional validation.

### 2. [P1 | game report + observed HLE gap] LEGO Worlds: NGS2 rack lifecycle must produce meaningful handles/state

- [LEGO Worlds #5210](https://github.com/shadps4-emu/shadPS4/issues/5210) reports audio as noise and NGS2 rack IDs such as \`0x2001\`/16; [\`src/core/libraries/ngs2/ngs2.cpp\`](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/ngs2/ngs2.cpp) currently has \`sceNgs2RackCreateWithAllocator\` returning success on a non-null system while not visibly creating/updating the requested rack output handle. **This proves a state-contract gap, not that this alone explains noise.**
- Task: first establish public-supported option/handle ABI and the minimum rack lifecycle; implement only the confirmed subset (create/query/destroy or deterministic unsupported error) and ensure no uninitialized success outputs. **Do not attempt a complete NGS2 audio engine** in one PR.
- **Without a game, conditional:** new \`shadps4-open-test\` guest exercises create/query/destroy with two supported rack types, invalid IDs/pointers, repeated lifetimes and handle isolation; expected state/return codes need justified oracle. Synthetic PCM only if actual audio path exists. If ABI cannot be established, stop after a host regression test and feasibility report.

### 3. [P1 | recent game report + missing HLE implementation] Dead or Alive 5: Last Round: ULT queue semantics

- [Dead or Alive 5 #5353](https://github.com/shadps4-emu/shadPS4/issues/5353) (closed 2026-10-11) shows \`sceUltQueuePush\` / \`sceUltQueuePop\` hitting \`CommonStub\` and a later task-controller crash. Current source search finds no corresponding real implementation. Game causality is **plausible, not demonstrated**.
- Task: identify license-compatible public libSceUlt queue ABI; implement a **minimal, thread-safe producer/consumer FIFO contract** only if handle/layout, wait/timeout and error semantics are defensible. No arbitrary zero-success stubs; no made-up ABI.
- **Without a game, conditional:** guest queue test in \`shadps4-open-test\` with 1 producer, 1 consumer, bounded queue, FIFO, full/empty/block/unblock/timeout/shutdown; host race checks/TSan where possible. If no public ABI is available, mark **BLOCKED** instead of writing a speculative API.

### 4. [P0 | confirmed source path] God of War III Remastered: CLI patch lookup in provider subfolders

- [Issue #4630](https://github.com/shadps4-emu/shadPS4/issues/4630) gives concrete \`God_of_War_III_Remastered.xml\` example. Current [\`src/common/memory_patcher.cpp\`](https://github.com/shadps4-emu/shadPS4/blob/main/src/common/memory_patcher.cpp), \`OnGameLoaded\`, checks the requested direct path or \`PatchesDir / patch_file\`, but not \`PatchesDir / provider / patch_file\`; automatic patch scanning does use provider directories.
- Task: implement predictable safe resolution of an explicitly requested filename into installed provider directories, with deterministic handling of duplicate filenames and traversal attempts. Preserve absolute-path behavior and explicit-direct-path precedence.
- **Without a game:** generated XML and provider folder tree, host test of selected file and no-file/duplicate/traversal cases; a synthetic homebrew can be used to verify patch application end-to-end but is **not required** for the path-lookup contract.

### 5. [P1 | game report only] Dead or Alive 5: Last Round: chroma/stride boundary in video playback

- [DOA5 #4458](https://github.com/shadps4-emu/shadPS4/issues/4458) reports a green line at the bottom of cutscene video, even after an earlier general AvPlayer fix; issue is closed and current reproducibility is **unconfirmed**. Target [\`src/core/libraries/avplayer/\`](https://github.com/shadps4-emu/shadPS4/tree/main/src/core/libraries/avplayer). This is **not** the same as batch #22's FSR presentation scenario.
- Task: reproduce a lower-edge chroma/padding artifact using generated YUV420/NV12 videos with odd/even dimensions, explicit crop/stride and sentinel rows; diagnose actual mismatch before any fix. Distinguish decode output versus upload/presentation; don't guess an AvPlayer cause solely from an image.
- **Without a game:** self-generated small video, FFmpeg-backed host decode oracle; if exposed to guest, add \`shadps4-open-test\` AvPlayer playback and deterministic pixel/frame hash under lavapipe. If test remains green, create regression-test-only change.

### 6. [P1 | upstream open report; ownership check] Game update discovery for \`CUSA-app\` + \`CUSA-patch\`

- [Issue #3651](https://github.com/shadps4-emu/shadPS4/issues/3651) shows an installed base folder with \`-app\` suffix causing updates to be skipped; issue remains open. Source of title/update folder discovery may involve a separate frontend, **not yet established to be within shadPS4 core**.
- Task: trace selection of base content and update metadata; fix suffix handling *only where it actually lives*, preserving serial/title validation. **Stop/re-scope if ownership is in external QtLauncher instead of this fork**; do not invent a core change to satisfy the issue.
- **Without a game:** generated directory tree and synthetic \`param.sfo\` metadata for matching and nonmatching base/update pairs, asserting selected app version/patch mount path. Homebrew optional for an end-to-end patch-files visibility test; no real game content.

### 7. [P0 | observed memory safety] Reject oversized SELF/ELF \`PT_SCE_PROCPARAM\` segment before copying

- [Issue #5249](https://github.com/shadps4-emu/shadPS4/issues/5249) shows a fully synthetic ASan reproducer. Current [\`src/emulator.cpp\`](https://github.com/shadps4-emu/shadPS4/blob/main/src/emulator.cpp) \`ReadCompiledSdkVersion\` passes \`p_filesz\` into \`Elf::LoadSegment(u64(&param), ...)\` with a fixed-size local \`OrbisProcParam\`; [\`src/core/loader/elf.cpp\`](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/loader/elf.cpp) copies the requested count. The issue is closed but the vulnerable call pattern remains visible.
- Task: validate segment extent against destination structure and file, require exact/allowed size per public structure definition; avoid generic unsafe \`LoadSegment\` API changes without an audit.
- **Without a game:** synthetic harmless ELF/SELF headers with valid, short, oversized and truncated segment lengths; ASan/UBSan, process exit and normal version extraction oracle. No homebrew or GPU required.

### 8. [P0 | observed memory safety] Validate \`param.sfo\` table and string bounds before parsing

- [Issue #5250](https://github.com/shadps4-emu/shadPS4/issues/5250) contains synthetic examples. Current [\`src/core/file_format/psf.cpp\`](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/file_format/psf.cpp) copies the PSF header without checking buffer length and iterates \`index_table_entries\` without bounding the index table; key/data offsets and null-terminated strings are also unbounded. Closed report, still observed in source.
- Task: overflow-safe span validation for header, entries, offsets, text termination and supported field lengths; preserve valid signed/unsigned values and existing SFO encoding behavior. Do not conflate with batch #22's **trophy XML** parser.
- **Without a game:** generated valid SFO plus short header, inflated entry count, overflowed table offsets, unterminated key/value and short data. Verify rejected cases under ASan/UBSan and byte-for-byte successful roundtrip on normal fixtures. No guest/GPU necessary.

### 9. [P2 | reported, NOT diagnosed] Sackboy: A Big Adventure: abnormal 56-GiB virtual allocation request

- [Open issue #5224](https://github.com/shadps4-emu/shadPS4/issues/5224) reports a repeatable roughly **56-GiB** address-space allocation followed by a fatal allocation failure despite low reported in-game committed memory. Whether the size is a valid reservation, guest bug or corrupted argument is **unknown**.
- Task: isolate address-range and alignment checks, reservation-versus-commit accounting and predictable guest error behavior. Instrument a safe bounded failure path before attempting any allocation policy change; **do not** grant an invalid request or intentionally consume 56 GiB RAM to make a test pass.
- **Without a game: investigation/test-only initially:** host/guest scripted virtual reservation matrix (including large sparse/uncommitted allocations where the host permits), error and guard-region oracle. No compatibility fix justified without stronger evidence; label STOP/UNVERIFIED if no safe synthetic repro.

### 10. [P2 | historical game report, likely regression-test-first] The Last of Us Part II: direct-memory size / flexible memory accounting

- [Historical issue #789](https://github.com/shadps4-emu/shadPS4/issues/789), now closed, describes a guest halting on \`sceKernelGetDirectMemorySize\`. Current [\`src/core/libraries/kernel/memory.cpp\`](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/kernel/memory.cpp) returns \`MemoryManager::GetTotalDirectSize()\` and [\`src/core/memory.h\`](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/memory.h) exposes separate direct/flexible sizes. No current error in the returned total has been shown.
- Task: derive memory partition invariants from shadPS4 code and public docs, test initial value vs. flexible-memory configuration and PS4/Pro modes. **Regression-test-only if correct; do not hardcode a number extracted from one commercial game's binary** or change global memory layout on anecdotal evidence.
- **Without a game:** synthetic OpenOrbis guest queries size before/after permitted flexible-memory operations, asserting sensible accounting, stability and distinction between hardware modes only where supported by public ABI.

## Quick execution matrix

| # | Motivating title | No commercial game? | Oracle | Expected scope / gate |
|---|---|---|---|---|
| 1 | Bloodborne | **Yes** | host injected-clock + audio timing; optional homebrew | M, CPU timing environment |
| 2 | LEGO Worlds | **Conditional** | NGS2 guest handles/lifecycle | M/L, public ABI must exist |
| 3 | Dead or Alive 5 | **Conditional** | ULT FIFO guest concurrency | L, public ABI must exist |
| 4 | God of War III | **Yes** | XML path-selection unit tests | S |
| 5 | Dead or Alive 5 | **Yes, if synthetic reproduction** | generated video/frame hash | M, investigate first |
| 6 | Any game + updates | **Yes, if owned here** | synthetic base/patch directories | S/M, launcher ownership |
| 7 | General game loader | **Yes** | synthetic ELF + ASan | S |
| 8 | General metadata loader | **Yes** | synthetic SFO + ASan | S/M |
| 9 | Sackboy | **Investigation only** | bounded virtual-address tests | M, no proven fix |
| 10 | The Last of Us Part II | **Yes, test-first** | guest memory APIs | S/M, stale report |

**Suggested agent execution now:** #7, #8, #4, #1, then #5 and #10. #6 only if code ownership is established. #2/#3 only after ABI gate. #9 research/test-only. Share no game dumps; attach linked homebrew-test PRs where an executable is needed.

## Exclusion and freshness notes

- **Not repeated:** batch #21's PNG/RTC/PlayGo/SharePlay/RemotePlay/VideoRecording/IME; batch #22's trophy, SDL queue, FSR, gamma, game time IDs, save importer, Wayland DPI, per-game language.
- The public upstream PR inventory included work on GPU commands [#5190](https://github.com/shadps4-emu/shadPS4/pull/5190), Vulkan pipeline caching [#5246](https://github.com/shadps4-emu/shadPS4/pull/5246), voice [#5149](https://github.com/shadps4-emu/shadPS4/pull/5149), audiodec CPU [#5289](https://github.com/shadps4-emu/shadPS4/pull/5289), save data [#5226](https://github.com/shadps4-emu/shadPS4/pull/5226), and file-system stat [#5309](https://github.com/shadps4-emu/shadPS4/pull/5309). This explains the deliberately narrow test gating for most remaining game issues.
- Source verification showed [\`internal__Foprep\` #4698](https://github.com/shadps4-emu/shadPS4/issues/4698) **already has** a null-return on current main; it is excluded. Also [empty custom patch folder #4915](https://github.com/shadps4-emu/shadPS4/issues/4915) now has a defensive missing-index check; not listed as a live bug.
- **The final selection is a planning shortlist, not a statement that all ten are currently broken**. Closed issues can be outdated, upstream changes can supersede this snapshot, and named games remain unverified until independently tested.
