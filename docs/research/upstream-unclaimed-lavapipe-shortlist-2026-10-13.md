# Ten upstream-only candidates for agent implementation and lavapipe verification

**Snapshot:** 2026-10-13 (Europe/Berlin). **Status:** proposal only; none of the ten fixes is implemented by this PR. **Source of candidate history:** exclusively `shadps4-emu/shadPS4` and the Supabase project `shadPS4 upstream watch`. The destination fork is used only to store this document.

## Method and anti-collision policy

- Supabase: upstream hourly and daily observations on **11–12 October 2026** (the monitor started 11 October). The 12 October daily report records **40 distinct PRs with activity** in its 30-hour window, including 9 merged and 16 still open (13 ready, 3 draft). The latest observed hourly snapshot at 12 October 14:06 UTC reported 77 open PRs; a later live GitHub search returned 78 open PRs. The difference is expected because the searches were taken at different times.
- GitHub: current upstream open-PR inventory plus targeted full-history PR queries for each chosen area, existing issue reports, and current source in three representative locations. A query with no relevant open PR is **negative public evidence**, not proof that nobody is working privately.
- **Exclusion:** shader wave/PM4 instruction expansion, GPU-written shader coherence, depth clamping, image/texture/buffer cache coherence and GC, savedata backup callbacks, kernel equeue, USB storage, shadNet NPAuth, resolver overrides, voice I/O and oversized compute workgroups. There is already recent or open work in/near those areas (e.g. upstream #5355, #5357, #5190, #5363, #5280, #3396, #5226, #5192, #5338, #5350, #5362, #5149, #5168). Also exclude recently closed or already implemented requests, and input-to-quit-dialog work near #5235.
- Candidates below are deliberately **scoped to individual behaviors**, not a claim that a whole subsystem is unowned. Before taking *each* item, an agent must refresh upstream main, open PRs, merges, and its linked issue. If a substantive overlapping PR/commit has appeared, **stop and mark SUPERSEDED**.
- One implementation = one separate, small PR **in the personal fork**, not automatically an upstream PR. Compare against current upstream main, not the potentially older default branch of the fork. No proprietary PS4 game or firmware is required for the **agent-side acceptance tests**. A real-game follow-up may be valuable, but is **not** part of lavapipe sign-off.
- Lavapipe is a **CPU Vulkan ICD**. It can verify Vulkan paths, command submission, shaders, presentation under a compositor, and open homebrew guest results. It cannot prove physical GPU performance, RADV/NVIDIA behavior, Liverpool GCN hardware semantics, or that an arbitrary commercial title is fixed. For non-renderer tasks, tests execute in the same Linux/lavapipe environment but the meaningful oracle is a deterministic host or synthetic guest test.

## Ranked shortlist

| Rank | Task and upstream evidence | Effort / expected value | Automated acceptance on Linux + lavapipe |
| --- | --- | --- | --- |
| 1 | [Persist an explicitly unbound screenshot hotkey](https://github.com/shadps4-emu/shadPS4/issues/5328) | S / high confidence, UX | Save `unmapped`, restart/reload settings, inject F12 and verify no screenshot action; assign a new key and verify one action |
| 2 | [Per-game console-language override precedence](https://github.com/shadps4-emu/shadPS4/issues/4524) | S–M / high | Different global and game language configs; a synthetic guest queries the effective console language; other title retains global |
| 3 | [Correct the GPU timestamp clock domain](https://github.com/shadps4-emu/shadPS4/issues/4981) | M / potentially high | Timestamp producer/consumer homebrew and deterministic clock-domain tests; reject wall-clock nanoseconds masquerading as guest GPU ticks |
| 4 | [Bounded shutdown after a fatal error](https://github.com/shadps4-emu/shadPS4/issues/4854) | M / high | Inject a controlled fatal error with lavapipe initialized; process exits nonzero, logs once, no stuck worker/window, timeout enforced |
| 5 | [Bounded preflight reporting for malformed game metadata](https://github.com/shadps4-emu/shadPS4/issues/4980), related [#4998](https://github.com/shadps4-emu/shadPS4/issues/4998) | M / medium-high | Synthetic valid/truncated/invalid SELF/ELF and param.sfo fixtures; stable reason codes and no crash/hang; positive control |
| 6 | [Optional Rec.709 output transfer function](https://github.com/shadps4-emu/shadPS4/issues/5187) | S–M / medium | Pure shader color-ramp reference comparisons on lavapipe, endpoints/threshold checks, old sRGB path byte-identical within stated tolerance when off |
| 7 | [Wayland fractional/2× scale drawable-size handling](https://github.com/shadps4-emu/shadPS4/issues/4886) | M / medium | Weston headless scale-1 and scale-2 sessions with lavapipe; SDL drawable, swapchain extent, framebuffer and screenshot dimensions consistent |
| 8 | [Make the IME initial-shift rule explicit and testable](https://github.com/shadps4-emu/shadPS4/issues/4403) | M / medium | Synthetic IME dialog input: first-letter capitalization only when applicable, no uppercase after cursor edits by accident, Unicode unchanged |
| 9 | [Opt-in, bounded LLE-call tracing](https://github.com/shadps4-emu/shadPS4/issues/2033) | M / medium for debugging | Mock LLE dispatch with known imports; count/names/call-return events correct, off-by-default produces no events, no proprietary modules |
| 10 | [Avoid unnecessary semaphore refresh when no callbacks are pending](https://github.com/shadps4-emu/shadPS4/issues/5079) | S spike / low-to-uncertain | Instrument refresh count and completion callbacks under lavapipe; empty queue no refresh **only if** all wakeup/timeline tests still pass |

### 1. Explicitly unbound hotkeys — #5328

**Evidence:** As of 9 October, an unset screenshot binding could revert to F12 after restarting a game. Open upstream PR query for “Customize Hotkeys” returned no matching PR; the earlier generic hotkey-format PR #4000 (February) was closed without merge. No 11–12 October upstream change was identified as addressing the `unmapped` persistence rule.

**Agent task:** Reproduce via the actual serialization/deserialization plus dispatch path; distinguish “not present → use default” from “explicitly disabled → no binding”. Fix the smallest point of ambiguity, not the whole keymapping architecture. Preserve migration of older configs and an explicitly set F12.

**Oracle:** save/reload twice, inject synthetic SDL key events; zero screenshot requests when disabled; exactly one when enabled; no regression to other hotkeys. Run an Xvfb/lavapipe startup smoke to confirm no renderer coupling. **Stop** if the current upstream behavior already passes these tests.

### 2. Per-game language — #4524

**Evidence:** An upstream issue reports game-specific language ignored in favor of global language. A targeted upstream open-PR query for “per-game language” and “console language” returned no relevant PR. Upstream #5339 adds an EmulatorSettings assignment operator for shadlauncher, but its PR description explicitly says it does not change qtlauncher behavior; recheck nevertheless before implementation.

**Agent task:** Trace when global vs title-specific config is loaded and when the guest-facing system-language values are cached. Avoid broad settings refactors. Build a tiny license-free homebrew caller for the language query actually implemented in this revision; verify the API first rather than assuming a particular symbol.

**Oracle:** run title A with global language X and override Y, then title B without override; get Y and X respectively, in both launch orderings. Save/relaunch config and ensure the override is not rewritten globally.

### 3. GPU timestamp units and meaning — #4981

**Evidence:** Current upstream `src/video_core/amdgpu/pm4_cmds.h` still defines `GetGpuClock64()` using `high_resolution_clock` nanoseconds, while adjacent `GetGpuPerfCounter()` converts CPU cycles using the guest GPU clock frequency. No matching open PR for GetGpuClock64 / GPU timestamp was found. The issue's Diablo III dynamic-resolution diagnosis is plausible but **not independently reproduced here**.

**Agent task:** First write an oracle for the units returned by relevant EOP/RELEASE_MEM GPU-clock packets. Separate (a) units/frequency conversion from (b) timestamp placement relative to GPU completion; do not pretend a simple 0.8/0.911 scaling makes the latter accurate. Propose the smallest defensible change after inspecting the exact packet paths and host scheduler. Do not modify unrelated PM4 parsing (open #5190).

**Oracle:** deterministic clock-domain conversion tests plus a freely built synthetic guest comparing two ordered timestamp publications; repeat under lavapipe with a known controlled delay and assert monotonicity, nonnegative deltas, and order. **Partial-only** if proper execution-time semantics cannot be obtained; lavapipe cannot certify a commercial game's DRS.

### 4. Fatal-error shutdown — #4854

**Evidence:** Open issue for emulator remaining frozen after fatal VRAM errors, with no upstream PR matching a dedicated fatal-error shutdown change in targeted searches. Unlike recently active texture-cache allocation/GC work, the proposed unit is **the application lifecycle and termination path**.

**Agent task:** Add a test-only fault injection at an existing checked fatal boundary (or a small test seam) without turning recovery into a general state machine. A fatal condition must be communicated to the main/event thread and cannot strand joiners; preserve useful logs and nonzero exit. Never emulate “recovery” from device loss unless the implementation supports it.

**Oracle:** initialize SDL and a lavapipe Vulkan context, trigger one controlled fatal after workers exist, assert process exits under an explicit timeout and final log records the reason. Include a nonfatal control and a repeated-run leak/deadlock check. Device-loss-on-real-GPU remains unverified.

### 5. Metadata validator with honest scope — #4980 / #4998

**Evidence:** Open requests for corrupt-dump identification and fuller ELF validation; no corresponding open PR found by targeted “bad dumps” / “ELF validation” searches. Note that separate malformed SELF and SFO reports #5249/#5250 were **closed as not planned** on 4 October: do not resurrect them as an unrequested parser-wide security rewrite.

**Agent task:** A *read-only* preflight checker over headers, sizes, section offsets, required metadata and discoverable title structure, reporting precise failures. The result must be “structurally valid / invalid / incomplete / unknown”, **never** “authentic Sony dump” or “all assets uncorrupted”: there is no trusted reference hash for arbitrary dumps. Cap recursion, memory and time spent. Make it callable without rendering and optionally integrate one concise launcher diagnostic if trivial.

**Oracle:** entirely generated positive and malformed fixtures (e.g., truncated header, size overflow, missing param.sfo, duplicate conflicting metadata, symlink cycle), stable machine-readable verdict and bounded runtime. Build and smoke-test under lavapipe even though no GPU path is involved.

### 6. Rec.709 output option — #5187

**Evidence:** Upstream issue requests a Rec.709 transfer option. Current `src/video_core/host_shaders/post_process.frag` contains the preexisting sRGB/gamma path and **no Rec.709 branch**. No relevant open PR in post_process / Rec.709; older gamma work #1756 merged in 2024.

**Agent task:** Implement an explicit opt-in with a well-defined mapping into the existing presenter and settings. Keep the default path unchanged. Be precise about transfer functions versus limited/full output ranges and color primaries; do not reclassify a gamma option as true HDR/Rec.709 color management.

**Oracle:** compile the fragment shader on lavapipe; feed a linear ramp and check known transfer thresholds/endpoints, continuity, alpha preservation, sRGB-input and HDR bypass cases. Reference CPU implementation and tolerance recorded, with before/after default-path control.

### 7. Wayland scale and drawable extent — #4886

**Evidence:** Open issue reports a 200% Wayland desktop producing a half-resolution output; no recent/open Wayland scaling PR found. Earlier upstream #4539 fixed first-frame HiDPI SDL pixel-size cache during fullscreen startup, **not demonstrated** to resolve the issue's compositor scaling scenario.

**Agent task:** Identify exactly where logical SDL window coordinates are confused with pixels: display scale, size-in-pixels, Vulkan surface extent, overlay and screenshot. Change only the misplaced conversion, do not multiply dimensions twice. Test fullscreen, borderless and a windowed mode separately.

**Oracle:** a repeatable Weston headless Wayland compositor with scale 1/2 (or the simplest available nonprivileged compositor), lavapipe ICD and tiny synthetic window/render; programmatically record SDL logical vs pixel size and swapchain/image extents, assert the correct DPI relationship with no double scaling. **BLOCKED**, not PASS, if no Wayland compositor can be run.

### 8. IME initial shift — #4403

**Evidence:** Longstanding open IME feature list; the last directly relevant known PR #3973 merged layout/metrics changes in June, and targeted open-PR searches show no active work on “IME uppercase”. Select **only initial uppercase behavior**, not an entire non-Latin keyboard or gesture system.

**Agent task:** First determine the current IME field flags, shift state and user-entry semantics. Apply automatic initial uppercase only in permitted text modes; no automatic transformation of prefilled text, passwords or arbitrary Unicode codepoints. If PS4 semantics are ambiguous, keep it behind a conservative condition and document what is inferred.

**Oracle:** synthetic guest invokes open/type/edit/close IME sequences; assert uppercase on eligible first Latin letter, correct next-letter behavior, preserved lowercase-only/password modes and unchanged non-Latin input. Xvfb/lavapipe UI smoke; label hardware-console fidelity as unverified.

### 9. Limited LLE-call tracing — #2033

**Evidence:** Open issue asks for visibility into LLE exports that otherwise execute silently. Targeted open-PR search for LLE logging/tracing found no relevant active work. This proposal is **instrumentation**, distinct from upstream HLE implementation work.

**Agent task:** Intercept only the existing LLE dispatch boundary and log resolvable library/NID, call and result metadata on an explicit opt-in. No generic arbitrary argument dumps (pointers may point to credentials or huge/invalid guest buffers), no per-instruction tracing, and no changes to ABI or dispatch order. Offer a bounded event budget/filter to prevent unmanageable logs.

**Oracle:** fake/self-built LLE dispatch target with known exports; assert names/IDs, event ordering and optional filter. Verify zero events and unchanged function return with tracing disabled. A lavapipe homebrew smoke is optional if the harness truly reaches this code path; do not claim real Sony module coverage from a mock.

### 10. Empty-queue Vulkan semaphore refresh — #5079

**Evidence:** Current upstream `vk_scheduler.cpp` still calls `work_semaphore.Refresh()` in `Scheduler::PopPendingOperations()` before checking `pending_ops.empty()`. Open issue asks whether that does unnecessary work. The previous related PR #4748 (July) was a concurrency lock fix, not this proposed empty-queue rule; targeted current open-PR search for the function returned zero matches.

**Agent task:** **Investigate before modifying.** Prove whether Refresh has required global side effects (timeline progress, priority wakeups) even when this callback queue is empty. If not, skip only demonstrably redundant calls; preserve the mutex/wait ordering from #4748. Avoid generic scheduler tuning.

**Oracle:** add counter/trace around Refresh, run existing completion, callback-order, multiple-producer and empty-queue tests under lavapipe; compare old/new callback completion and synchronization validation. If savings are negligible or semantics unclear, **stop with an evidence note; no code PR**. Do not claim a GPU performance improvement from lavapipe.

## Agent execution contract for each task

1. Fetch **current upstream** main and current PR inventory; search the issue title, implementation symbols and changed file(s). Record upstream SHA and competing PRs. If a relevant active or just-merged change appears, mark `SUPERSEDED` without editing.
2. Establish a failing before-test or a measured pre-change baseline, with generated inputs and **no proprietary game/sysmodule files**. Keep the comparator clean and reproducible.
3. Implement only the scoped fix/feature in a separate fork branch. Run formatting, applicable build/unit tests, and the specific lavapipe (Mesa llvmpipe Vulkan) test. Capture GPU device name, Mesa version, guest/test commit, run modes, and explicit exit codes.
4. Compare old and new behavior, including a negative control and at least one regression check. Classify `PASS`, `FAIL`, `BLOCKED`, `NOT APPLICABLE` distinctly; a passing CPU-only unit test must not be marketed as a lavapipe runtime integration pass.
5. After review, open **only a fork PR** containing code, evidence and the clearly stated validation limits. Don't push to upstream or merge automatically.

## Why other tempting ideas were not selected

Recent upstream activity shows direct overlap with the following subjects: CE/DE ordering and GPU-written shaders (#5355); Vulkan depth clamp (#5363); draw predication and PM4 features (#5190); wave32/wave64 transformation (#5357); compute workgroup size limits (#5168); image/buffer ownership (#5280); texture GC (#3396); user-data, index-buffer and color-export details (#5358/#5279). Non-GPU overlaps include network resolver (#5362), NPAuth (#5350), backup callbacks (#5226), USB (#5338), voice (#5149) and kernel I/O event queues (#5192). Prior hotkey, IME and Wayland PRs were checked as historical comparisons, not treated as current work.

**Limit of this screening:** public upstream PR/issues and the 11–12 October Supabase monitoring history only, not private branches, external contributor plans or downstream fork activity. The result is a *prioritized, collision-screened candidate list*, not 10 verified unfixed bugs. The agent should deliberately abandon a candidate when its before-test already passes or a newer upstream fix appears.
