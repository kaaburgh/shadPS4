# Bloodborne death/reload harness reconstruction (V1)

Base: exact E0 2437a5538bbc4fdffed8cf04bdb06ba08f91e095. Work is isolated in
research/uma-e0-deathloop-harness; E0 and detached E0b stay unchanged.

## What actually ran

Historical launch: tools/run_bb_death_reload_benchmark.py in the independent
investigation repository starts the emulator with guest environment variables
SHADPS4_BB_LOAD_DIAG=1, SHADPS4_BB_DEATH_LOOP=1, *_CYCLES, *_LEFT_Y=255,
*_TIMEOUT_S, *_LOAD_DIAG_DIR. Source src/input/bb_load_diag.cpp owns the state
machine. Python samples /proc/perf, tails lifecycle records, captures screenshots
with work/bb-1.09-exploration/diagnostics/x11_control.py and scores graphics with
score_bb_screenshots.py. Optional analyze_bb_vmas.py/analyze_bb_resources.py are
resource analyses, not lifecycle oracles. Observation-marker files can gate the
next movement until the Python screenshot capture has completed.

Source inspected: shadPS4-bb-post5100-lifecycle at 53729b37, shadPS4-source at
b0693d72, input replay commit 79866f24 and current E0 pad/fs/IPC source. Other
historical instrumentation trees are read only. The retrospective is
/home/ubuntu/bb-shadPS4-correctness-instrumentation/docs/experiments/notes/bloodborne-automation-harness.md.
Actual prior command/cycles/lifecycle/pad records were checked in
.../docs/experiments/evidence/bb-death-reload-validation-direct-offline-20260913-013000.
It records 3 completed cycles, durations approximately 34.24,33.17,35.73 seconds,
exit 0. The retrospective additionally reports a 10/10 baseline. These are old
reported/runtime observations, not evidence for current upstream.

Startup is automated: splash-hide plus first pad read +2s -> Cross; FromSoftware
logo asset close +5s -> Cross on default Play Offline; read-only userdata0010
close +1.5s -> Cross on Continue. A bounded extra-Cross recovery branch handles
offline-notice differences. Gameplay gate is compound save read + loading asset
close +2 PlayGo speed calls + sound bank open. It is a benchmark control proxy,
not proof of first visible or controllable frame. Initial checkpoint/save and
facing were operator-prepared once; navigation to a new checkpoint was not a
universal automatic capability.

One cycle: gameplay-ready +200ms -> LeftY255; backup0000 unlink followed by
userdata0000 write/close is a death *candidate*, confirmed only by sound-bank
close or post-death PlayGo transition; then neutral LeftY128. Sound bank
sprj_xm24.fsb close/open defines load start/end. Subsequent non-0000 backup token
and matching userdata write/close defines next-gameplay-ready. Start next cycle
with a fresh LeftY255 after the observation hold. Count N confirmed cycles, fail
on watchdog, then quit. A continuous direction hold is not this state machine.

The actual cycle has event gates plus timing settles; input is controller-level
injection, not .bbpad replay. FS callbacks in kernel open/close/unlink, PlayGo,
splash-hide, pad-read and controller Tick were its source dependencies. They are
not on E0 and will not be reintroduced as Bloodborne emulator hacks.

## Replay premise checked

79866f24e9291998cb61d62fee71c7652c8d7164 is NOT an ancestor of E0. Their merge base
is 7a8caf12b60133a3303acef31b055c1d0ebe08c5. E0 has no input_replay.cpp/h and no
--input-record/--input-replay CLI; scePadStartRecording/StopRecording are stubs.
Therefore the requested current-upstream replay smoke cannot literally use that
API at this exact base. Do not silently change the base or port 878 lines.

The historical implementation has --input-record, --input-replay,
--input-replay-ordered, --exit-after-replay and --input-replay-self-test. F10 arms
the next GNM submit-done boundary. It captures returned guest pad calls at
(relative GNM submit-done count, read ordinal), with API/capacity and 120-byte
OrbisPadData samples. It is not a host-time replay. Strict mode aborts desync;
ordered mode ignores position mismatch but still checks API/capacity. Completion
returns neutral forever or exits; F10 does not restart a completed replay. There
is no automatic death/loading reset or loop, and no lifecycle oracle in replay.

Existing recordings (preserved, never overwritten):

* bloodborne-safe-route-v1.bbpad: 120076 bytes,811 calls/end progression811,
  sha256 2a53d1b40b518c052627dfd85dbeee9427046032d3d435e32417a904f3bcb9d3.
  Dominant inputs are neutral and four stick directions. Not a proven death loop.
* bloodborne-long-route-20260911-142827.bbpad: 7411148 bytes,50075 calls/end53321,
  sha256 e4a9e236c855b10b54f0843142d3754e70d31c5e1267100597f5f04abeb90b5f.
  Manual recording meta says source79866f24, same disposable target and save.
* numerous self-test/invalid .bbpad controls in diagnostics are not game routes.

Version1 format: 24-byte little-endian S4PADRPL/u32version1/u32clock1/u64payload
header; Call tag1,u64progression,u32ordinal/api/capacity/count, count*120 samples;
End tag2,u64progression,u32ordinal,u64call_count. The files match the old reader;
current upstream has no reader. Converting guest-call order to X11 wall-time
would not preserve this timing contract, so that conversion will not be claimed.

The existing screenshot oracle detects CLEAN/EXPLODED graphics at a known post-
respawn view; it is not a death/reload detector. Templates/screenshots remain
useful independent checks, but elapsed time or graphics score alone cannot
fabricate a cycle.

## Bounded implementation strategy before coding

Keep emulator source and all E0 census meanings unchanged. Reuse the same prepared
save/checkpoint in independent profile copies. Prefer an external Linux inotify
observer for existing host-backed sound/menu/save open,close,unlink events, plus
existing INFO splash/PlayGo logs. Current HostFile owns a real IOFile and maps
SaveInstance's directory directly, so this is a testable route to old semantic
markers without guest-memory instrumentation. Raw kernel notifications are host
facts, not exact guest callback times; attribute under a dedicated profile and
single emulator, reject overflow/missing markers and bound every stage. Validate
the route before claiming equivalence. Use existing X11 input/screenshot helpers;
add persistent press/release orchestration so each confirmed death releases input
and each next-start explicitly starts the next iteration. No fixed-time success.
A source-dependent .bbpad loop is unnecessary if this external oracle works.
