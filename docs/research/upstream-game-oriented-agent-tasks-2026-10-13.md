# Second batch: 10 game-oriented, agent-sized upstream candidates

**Prepared:** 2026-10-13. **Type:** shortlist / planning only; no emulator fixes, runtime claims or tests are included.  
**Upstream baseline:** [shadps4-emu/shadPS4 `23c41333822fe8eca4c609b43119180af63ae4ac`](https://github.com/shadps4-emu/shadPS4/tree/23c41333822fe8eca4c609b43119180af63ae4ac) (2026-10-12 21:07 UTC).  
**Upstream history:** GitHub open PR inventory (78 at inspection), recent activity through 2026-10-12, issues, source files, plus Supabase `shadPS4 upstream watch` hourly/daily notes from 2026-10-11/12. **No fork history used to select tasks.**  
**Distinct from previous shortlist:** [kaaburgh/shadPS4#21](https://github.com/kaaburgh/shadPS4/pull/21). None of the ten here repeat its PNG/RTC parsing, PlayGo, SharePlay, RemotePlay, VideoRecording, or Unicode IME candidates.

## How the priority was chosen

Prefer (a) recognizable game or measured compatibility symptom, (b) no known upstream PR addressing that **specific mechanism**, (c) a limited change that an agent can both make and verify without commercial games, and (d) an independently checkable result via a synthetic PS4 executable under Mesa **lavapipe**. This is a *screen against public work*, not a guarantee that no one has a private branch. A closed issue or a legacy game report is not itself proof the bug survives on current main.

**Important distinctions:**
- **Observed:** cited code path visibly has a defect or lacks a behavioral guarantee. **Reported:** an upstream user observed a game symptom. **Hypothesis:** that mechanism causes the game symptom. These are deliberately kept separate.
- A homebrew/lavapipe test may verify an emulated API path; it does **not** verify original PS4 ABI or prove that the named game is fixed. A hardware-dependent cause cannot be proven or disproven by lavapipe.
- **No proprietary SDK/PRX, Sony files, commercial game dumps or copyrighted test fixtures.** Generate your own tiny application data, trophy XML, image buffers and presentation frames. All tests must be lawful to distribute. If a scenario cannot run without proprietary material, stop that candidate rather than labeling a unit test as a lavapipe integration pass.
- The prior PR's strict reproducibility contract also applies: negative control first; machine-readable output; exact SHAs, driver, environment, exit status, timeout, validation logs; PASS/FAIL/UNVERIFIED separated.

## Candidates (ranked)

### 1. [Game-backed; P0] Trophy metadata errors must not masquerade as successful registration/query
**Motivation:** [Fist of the North Star #5359](https://github.com/shadps4-emu/shadPS4/issues/5359) reports failure around trophies on startup (report only; no cause established).  
**Observed source:** [`np_trophy.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/np/np_trophy.cpp), e.g. `sceNpTrophyGetGameInfo` / `GetGroupInfo` return `ORBIS_OK` after failing to parse the trophy XML. The reporting code can therefore leave output structs uninitialized while claiming success.  
**Agent job:** preserve the successful flow; make missing/malformed XML, missing save metadata and the declared errors deterministic, without inventing Sony return codes. Verify output structs are valid/zeroed or untouched according to documented ABI. Limit to trophy metadata/query functions, not NP authentication.  
**Lavapipe oracle:** homebrew creates context and handle, queries a minimal *generated* `TROP_01.XML`, then malformed/absent XML; records error/result and a canary around output. Run with and without an icon. **GO** only if the open guest can exercise the same public HLE path; otherwise UNVERIFIED.  
**Risk/effort:** S–M, high confidence of incorrect success path, low confidence that it explains #5359.

### 2. [Game-backed; P0] Trophy notification lifecycle must not stall the guest
**Motivation:** [Lords of the Fallen #3996](https://github.com/shadps4-emu/shadPS4/issues/3996): freezes after trophy popup, disappears when notifications are disabled. This is a correlation, not root-cause proof.  
**Observed source:** [`trophy_ui.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/np/trophy_ui.cpp): the Draw path replaces or resets `current_trophy_ui` while inside a method of that object, and `AddTrophyToQueue` also resets the active UI under its mutex. This merits a focused lifetime/reentrancy audit.  
**Agent job:** first reproduce popups/queue transitions from a synthetic trophy event stream; instrument lifetime/destruction and thread dispatch; fix only any demonstrated re-entrant destruction, queue hang or lost completion. Prefer a safe deferred transition over large UI refactoring.  
**Lavapipe oracle:** synthetic guest raises 2–5 trophy events, then writes a subsequent heartbeat and exits; compare popup enabled/disabled, immediate close/reopen, rapid events, and a concurrent autosave *marker* (no actual game assets). Record event count, elapsed time and heartbeat.  
**Risk/effort:** M. UI needs Xvfb/ImGui and a deterministic short test duration.

### 3. [Game-backed; P0] Fix trophy-popup SDL audio initialization and resource ownership
**Motivation:** same [Lords of the Fallen #3996](https://github.com/shadps4-emu/shadPS4/issues/3996), but a **separate** source mechanism from item 2. This one should land as a small independent change only after checking if the lifecycle task has touched it.  
**Observed source:** [`trophy_ui.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/np/trophy_ui.cpp): `if (SDL_WasInit(SDL_INIT_AUDIO) != 0) { SDL_Init(SDL_INIT_AUDIO); }` appears inverted for an uninitialized subsystem. Constructor opens playback devices and streams; destructor closes/quits conditionally according to configured backend rather than tracked ownership.  
**Agent job:** test the SDL uninitialized/already initialized cases and both SDL/OpenAL configurations. Initialize when needed, track whether the popup acquired a device/subsystem reference, and ensure each stream/device is released exactly once without stopping the game's audio. Never infer a game freeze fix solely from the source defect.  
**Lavapipe oracle:** under SDL dummy audio + lavapipe and a generated trophy event, verify popup appears, no SDL error/resource leak, no hang; repeat across popup cycles and disabled popup mode.  
**Risk/effort:** S–M. No overlap with the current `libSceVoice` PR #5149's files.

### 4. [Game-backed; P1] Bound SDL playback queue without audibly destructive flushes
**Motivation:** [Bloodborne audio regressions #4182](https://github.com/shadps4-emu/shadPS4/issues/4182) and [repeated queue overflow #3291](https://github.com/shadps4-emu/shadPS4/issues/3291) (the latter is from an older build; do not assume it still reproduces).  
**Observed source:** [`sdl_audio_out.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/audio/sdl_audio_out.cpp), `ManageAudioQueue` clears the entire pending stream on threshold, plausibly producing discontinuities.  
**Agent job:** first generate a repeatable bounded-latency burst/consumer-lag case and prove that the current implementation drops entire queued content. Compare minimally invasive policies (bounded backpressure / selective drop / underrun handling) while preserving timing and bounded latency; only implement a policy with demonstrable objective improvement. **Avoid changes in `audioout.cpp`**, which is touched by separate upstream PR #4721.  
**Lavapipe oracle:** synthetic guest emits alternating-tag PCM chunks; SDL dummy/loopback captures output queue telemetry and sequence gaps; check max latency and exact sample continuity under controlled host slowdown. Audio-output correctness, not FPS, is the metric.  
**Risk/effort:** M. Impact on Bloodborne remains a hypothesis; not an audiodec/Voice rewrite.

### 5. [Game-backed; P1] FSR presentation path: synthetic resizing and lifecycle crash test
**Motivation:** [Uncharted 2 FSR crash #5107](https://github.com/shadps4-emu/shadPS4/issues/5107). No claim that FSR is the cause of its otherwise game-dependent jump sequence.  
**Source search:** target the FSR upscale/RCAS and intermediate image extent selection, **not** Vulkan pipeline cache or PM4 changes currently under review. The known report identifies only the user-visible switch, not a culprit function.  
**Agent job:** build a tiny render-to-present synthetic guest with fixed checkerboard/gradient frames, 720p/1080p outputs, FSR on/off, resize/recreate cycles and odd extents. Gather Vulkan validation messages and image hashes. Fix a concrete invalid-image/extent/synchronization defect if one is reproduced; otherwise land a test-only PR and close the investigation.  
**Lavapipe oracle:** distinct known pixel patterns and expected display bounds; zero validation errors, no device loss, stable frame count, no timeout; a negative control intentionally mismatches an extent.  
**Risk/effort:** M, **low confidence** the homebrew will reproduce the Uncharted crash. Re-screen pending presenter PR #4721 before work; stop on intersecting FSR code.

### 6. [Game-backed; P1] Optional Rec.709 display curve, with precise color regression fixtures
**Motivation:** [feature request #5187](https://github.com/shadps4-emu/shadPS4/issues/5187), primarily about darker scenes / PS4 display appearance relevant to Bloodborne. This is a preference/accuracy feature, **not** a proven Bloodborne bug.  
**Observed source:** [`post_process.frag`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/video_core/host_shaders/post_process.frag) currently has a fixed sRGB-like output transform and a gamma push constant, but no Rec.709 selection.  
**Agent job:** compare authored transfer function against an independently computed CPU reference; add an opt-in Rec.709 variant only if it can preserve HDR and existing default output unchanged. Keep UI wiring minimal. Explicitly document the distinction between transfer function, color primaries and limited/full range.  
**Lavapipe oracle:** render small grayscale and RGB ramp fixtures at representative input values; read image pixels, compare to CPU reference at tolerance and verify old default is byte-for-byte stable.  
**Risk/effort:** S–M. No evidence of an open PR targeting this shader at inspection.

### 7. [Game-backed; P1/test-first] Protect existing extended network clock behavior with a guest regression matrix
**Motivation:** [Everybody's Golf #3925](https://github.com/shadps4-emu/shadPS4/issues/3925) observed `Unsupported clock type 16` during an infinite loading screen. **Do not submit a fix for the old message:** current [`time.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/libraries/kernel/time.cpp) already explicitly handles `ORBIS_CLOCK_EXT_NETWORK = 16`.  
**Agent job:** add a compact guest-level test for `sceKernelClockGettime` and POSIX `clock_gettime`: types 16–19 and monotonic/realtime, valid/NULL output pointers, monotonicity, resolution and error codes. Investigate any remaining mismatch on current upstream; change code only on a reproduced failing case. This is a **test-coverage idea**, not a known unfixed issue.  
**Lavapipe oracle:** the app dumps two samples per clock plus return code and time delta; assert nanosecond ranges, monotonic advance, and stable error cases across repeated boots.  
**Risk/effort:** S, medium cross-platform value; not a duplicate of PR #21's `rtc.cpp` parsing/network tick tasks.

### 8. [Feature; P1/research gate] Import public-format trophy progress safely, without bundling console files
**Motivation:** [God of War III Remastered / trophy import request #4949](https://github.com/shadps4-emu/shadPS4/issues/4949), requesting Apollo-exported trophy progress. The issue links example console-derived files: **do not download or commit them** into the homebrew harness.  
**Agent job:** first establish a public, redistributable format description for the minimal trophy unlock records and identity matching. Prototype parsing of *generated* fixtures and rejecting truncated/duplicate/out-of-range records; preserve existing progress on errors and prevent partial corruption. **Stop at a documented feasibility assessment** if the format cannot be grounded without proprietary data; no blind byte-parser implementation.  
**Lavapipe oracle:** use synthetic fixture with known trophy IDs/timestamps and a generated trophy XML app; import to temporary user dir, query trophy flags/progress from homebrew twice (idempotence), then inject malformed data and assert unchanged state.  
**Risk/effort:** M–L, uncertain ABI/format; lower ranking because no real PS4 data can be used in CI.

### 9. [User-visible; P1/test-first] Wayland 200%-scale full-screen pixel size
**Motivation:** [Wayland scaling #4886](https://github.com/shadps4-emu/shadPS4/issues/4886) reports 4K at 200% scale presenting as 1080p. Applies to any game including Bloodborne, but **not a Bloodborne-specific report**.  
**Observed source:** [`sdl_window.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/sdl_window.cpp) already queries `SDL_GetWindowSizeInPixels` in at least one path; the issue may have been partly fixed.  
**Agent job:** test current main with headless/nested Wayland compositor offering 1×/2× logical/physical scales; distinguish window logical size, drawable pixel extent, swapchain and game resolution. Repair only a reproduced incorrect conversion and keep X11 1× behavior unchanged.  
**Lavapipe oracle:** capture reported output extent and screenshot pixel dimensions at 100/200%; assert physical dimensions, no repeated resize loop or unexpected device loss. If the machine cannot run a software Wayland compositor, mark **BLOCKED**, not PASS.  
**Risk/effort:** M, medium chance already fixed; check open presenter PR #4721 for functional overlap, stop if relevant.

### 10. [User-visible; P1/test-first] Game-specific console language overrides must reach the HLE API
**Motivation:** [language override issue #4524](https://github.com/shadps4-emu/shadPS4/issues/4524) reports Linux ignoring per-title console language; this can affect multilingual versions of well-known games, but no named game's failure was established in that report.  
**Observed source:** [`emulator_settings.cpp`](https://github.com/shadps4-emu/shadPS4/blob/23c41333822fe8eca4c609b43119180af63ae4ac/src/core/emulator_settings.cpp) loads global settings and per-title overrides. The report predates current config migration, so assume **uncertain/stale** until retested.  
**Agent job:** create two temporary title IDs with different game-specific JSON language values and a distinct global default, boot a tiny homebrew calling `sceSystemServiceParamGetInt` (and any relevant user-service accessor), and compare the returned language. Check both first launch and re-launch after changing the override. Fix only verified ordering/caching errors; do not touch IME UI or font policies (#21 and upstream #5326).  
**Lavapipe oracle:** JSON output with expected/actual language for global-only, title override, unknown title and restart. No commercial localization assets required.  
**Risk/effort:** S–M.

## Why some prominent reports are *not* on the list

- **Diablo III dynamic resolution / `GetGpuClock64` [#4981](https://github.com/shadps4-emu/shadPS4/issues/4981):** compelling issue with directly suspect code, but open upstream **[#5190](https://github.com/shadps4-emu/shadPS4/pull/5190)** already edits the same `src/video_core/amdgpu/pm4_cmds.h` file. Even though that patch affects different packet structs, this fails the conservative *avoid nearby ongoing work* gate. Reconsider after #5190 lands.
- **Assassin's Creed: The Ezio Collection speed [#4779](https://github.com/shadps4-emu/shadPS4/issues/4779):** video/pacing investigation overlaps the scope of open upstream **[#4721](https://github.com/shadps4-emu/shadPS4/pull/4721)**.
- **God of War III startup GPU synchronization [#4411](https://github.com/shadps4-emu/shadPS4/issues/4411)** and **Gran Turismo 7 compute PM4 crash [#5334](https://github.com/shadps4-emu/shadPS4/issues/5334)**: PM4/queue work is active (#5190, #5355, #5140).
- **Yakuza 0 /temp0 freeze [#4739](https://github.com/shadps4-emu/shadPS4/issues/4739)**, **Bloodborne savedata [#4803](https://github.com/shadps4-emu/shadPS4/issues/4803)**, and DLC mount issues: overlapping work exists in filesystem (#5309/#3771), save-data (#5226), or AppContent (#4891).
- **Texture/geometry bugs** in GTA V, Taiko, Wreckfest, WWE 2K16, inFAMOUS, The Last Guardian: too much live work in rendering, shaders, memory or PM4 (#3396, #5280, #5310, #5355, #5363 and others) and/or no isolated synthetic root cause.
- **Ghost Song SceFiosZlib0 crash [#5292](https://github.com/shadps4-emu/shadPS4/issues/5292):** lacks a demonstrable freely redistributable direct guest reproducer without relying on LLE proprietary sysmodule behavior.
- **Steam screenshot hotkey reset [#5328](https://github.com/shadps4-emu/shadPS4/issues/5328):** directly near the active `input_handler.cpp` work in upstream #5238. Not selected.
- **GPU-clock and physical-driver performance claims:** no lavapipe-only result may be labeled a gameplay FPS or Radeon/NVIDIA hardware fix.

## Acceptance and staffing notes

Treat these as **10 distinct scoped experiments**, **not 10 accepted bug explanations**. The two notification tasks (#2–#3) share a file: sequence them and reconcile before implementation; do not give them to independent agents with conflicting branches. Two explicit test-only-first candidates (#7 and #9) may terminate as tests or as NO-BUG on current main. Item #8 is an explicitly **gated feasibility feature** and may stop without code.

Every subsequent implementation PR must include (1) upstream overlap re-check by issue/title **and changed files**, (2) old-vs-new failing synthetic test on fixed SHAs, (3) exact command and a measured result under lavapipe, and (4) a statement separating actual synthetic correctness from inferred commercial-game impact. A red test on current main that cannot be reproduced with legal independent fixtures is grounds to defer, not to invent a patch.
