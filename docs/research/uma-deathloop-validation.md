# Automatic death/reload restoration — 2026-10-04

**YES: the automatic cycle is restored on current E0/upstream**, for the existing
prepared Bloodborne 1.09 checkpoint/profile. No interaction is required after
launch; each cycle separately starts movement, observes death/reload, releases
input, and explicitly starts the next cycle. Oracle confidence is a **strong
filesystem lifecycle proxy**, with independently inspected world and YOU DIED
screenshots. It is not a universal game-state API or exact first-visible-frame
measurement.

## Isolation and source

Branch: research/uma-e0-deathloop-harness.
Worktree: /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-deathloop.
Exact E0 base: 2437a5538bbc4fdffed8cf04bdb06ba08f91e095.
Reconstruction-before-coding commit: c2225087.
Original E0 and detached E0b were read only; old source trees and the dirty
investigation repository were not changed. `git diff <base> -- src CMakeLists.txt`
is empty. Existing E0 binary was reused because emulator source/build inputs did
not change; its SHA-256 is
647bc3fb6e073ad9565d9de6daec47321b2ca727308fa74040f229e71cf00aef.
No new emulator build was necessary. No E1/shared buffers, Finish/barriers,
MemoryTracker/BufferCache/page protections or shader changes were made.

## V1 and historical assets

[Reconstruction](uma-deathloop-reconstruction.md) records the old runner, invoked
helpers, exact callback-driven state machine, replay file formats/hashes and prior
3/3-cycle evidence. Key correction: 79866f24 and its input-replay component are not
on the E0 base; no compatible CLI/reader exists on current E0. It was not ported.
The old actual death loop was not using .bbpad. Both historical recordings remain
unchanged; converting their GNM/read-call clock to X11 wall time would not be an
equivalent replay. No new pad recording or game-memory detection patch was needed.

Reused: historical event sequence and controller semantics, prepared save/profile,
existing X11 helper's PID/window/capture/focus functions and the old runner's exact
TemporaryReadbacksOverride. Graphics screenshot oracle was inspected and retained
as a graphics oracle, not falsely repurposed as a lifecycle oracle. /proc/perf/VMA
helpers were inspected as optional profiling consumers and were not added to this
narrow restoration.

The actual current save bundle discovered through runtime file opens is
home/1000/savedata/CUSA00207/SPRJ0005. The historical flat SPRJ0005 path and app ID
CUSA03173 alone do not identify the current mounted bundle. The runner copies the
profile independently and records initial hashes of the bundle actually opened.
It excludes backup-directory traffic after selection. Original saves stay intact.

## Iteration and oracle

Startup: existing INFO splash-hide -> short settle -> Cross; actual FromSoftware
asset close -> five-second settle -> Cross on Play Offline; userdata0010 read
close -> 1.5-second settle -> Cross on Continue. A bounded extra-Cross recovery is
used only if no initial-load evidence follows.

Initial readiness requires userdata0000 read-close, nowloading2 close, sprj_xm24
bank open and at least two existing INFO PlayGo speed calls. After a four-second
observation hold, capture the world and explicitly press s (configured LeftY255).
Backup0000 unlink + userdata0000 write-close is only a candidate. Sound-bank close
after that candidate confirms death/load (strong proxy) and releases movement.
Sound-bank reopen, then a matching non-0000 backup/userdata write-close confirms
next-start. Record duration/evidence; either start a fresh next iteration after
the hold or stop at N. Watchdogs, API errors or notification overflow fail visibly.
Elapsed time alone cannot mark death or completion. Exact fall coordinates and
first-controllable-frame are unobserved. Supporting screenshots show actual death.

## Validation actually performed

18 focused tests passed: existing E0 analyzer tests, lifecycle negative controls,
per-cycle release/restart, timeouts, real inotify open/close/unlink controls, and
complete/gapped/dropped/truncated/unclean E0 transport controls.

| Stage | Result | Iteration durations, seconds |
| --- | --- | --- |
| exploratory prototype | 2/2 off, exit0; refined active-save attribution afterward | 36.950,34.264 |
| V2 refined upstream/E0 off | 2/2, exit0, no manual input | 37.126,39.401 |
| V3 same-seed repeat | 2/2, exit0; initial save hash set identical to V2 | 37.424,37.951 |
| V4 E0 Disabled, validated RAM route | 2/2, emulator exit0, no device-loss log evidence | 39.945,40.567 |

V2/V3 images confirm the same prepared world/facing before movement. V2 death
frames independently inspected include:
`uma-deathloop-v2-b/iteration-001-moving-022281.png` and
`iteration-002-moving-024219.png`, visibly showing YOU DIED. Loading and next-start
screenshots supplement the filesystem transitions. Near-ready screenshots can
still show the item/loading card, as in the historical harness; later input-start
images show the world. Do not promote the proxy to an exact visual boundary.

V4 raw transport: **54,836,488 records**, **5,702,994,752 bytes**, **0 dropped**,
clean shutdown, full contiguous sequence 0..N-1 verified using a bounded-memory
structural checker. Header source SHA matches exact E0 base; existing format and
producer semantics are unchanged. This check proves transport integrity, not
semantic shader-access or hazard classifications.

Negative transport evidence retained: ordinary disk route had 1,110,985 dropped
events; first RAM attempt had one. Lifecycle completed in both, but those census
streams are inadmissible for exact/negative claims. RAM output reduces the observed
loss, but does not guarantee zero loss on every run. Final runner treats any drops
as failure separately from lifecycle counts. No emitter, queue, sample filter or
GPU setting was changed to obtain the zero-drop capture.

The unchanged semantic analyzer was still running after ten minutes on the large
V4 stream and was explicitly deferred; raw data and metadata were preserved. The
final campaign runner defaults to the bounded structural check and offers
--analyze for the full unchanged semantic analysis. No full semantic result is
claimed. No final long Disabled/Precise campaign was run.

## Artifacts and retained private evidence

All persistent paths below use /media/ubuntu/UsbSSD447G/shadps4/runs/:

* uma-deathloop-v2-a (exploratory prototype)
* uma-deathloop-v2-b (V2)
* uma-deathloop-v3 (V3)
* uma-deathloop-v4 (lossy disk result)
* uma-deathloop-v4-ram (one-drop RAM result)
* uma-deathloop-v4-ram2 (zero-drop V4 result)

RAM artifacts are copied to persistent storage with matching SHA-256 verified
before removing task-owned transient RAM data. retention.json records source,
retained location and raw event digest. Private profiles are excluded from staged
RAM artifacts. Do not commit or return copied profiles, saves or keys. Return the
allowlisted launch/lifecycle/input/host-event/screenshots/private log and E0 capture
artifacts listed in [operator commands](../../tools/uma_e0/README.md).

Remaining setup: operator prepares the intended checkpoint/save/facing once and
starts an appropriate X11 display. Existing prepared profile worked automatically.
Remaining nondeterminism: filesystem/log observation latency, shader/OS/loading
variation, X11 input scheduling, warning/offline menu variants, and prepared-save
layout/input bindings. These are measured/proxy limits, not permission to invent
missing lifecycle states. Unseen markers stall/fail; vertex corruption is neither
suppressed nor used to exclude an iteration.
