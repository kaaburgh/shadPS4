# Observation-only UMA E0 census

Base upstream **f73709cada28e77523e37806fee5c454711d24ba**. Design was committed
first in **d8e993db**, with the verified command/Finish/backing-write corrections
in **76a61987**. No imports, synchronization changes, new protection/watchers,
barriers, Finish calls, upload/readback decisions or shader translation changes.

## Automatic death/reload campaign

The canonical runner is now on `research/uma-e0-deathloop-harness`, based exactly
on E0 2437a553. Original E0 remains unchanged. Emulator source changes: **zero**.
Current E0/upstream does **not** contain the historical .bbpad reader or CLI from
79866f24; that commit is on separate fork history. The old death loop actually
used event-gated controller input and filesystem/PlayGo callbacks, not .bbpad.
See [reconstruction](../../docs/research/uma-deathloop-reconstruction.md).

`--death-loop` automatically navigates the existing startup warning, offline
selector and Continue, then completes N death/reload iterations without Enter.
It watches real host files via Linux inotify and existing splash/PlayGo INFO logs.
Each iteration explicitly presses movement, confirms the compound death/load
sequence, releases movement during loading, observes the post-load save sequence,
and starts the next iteration after the observation hold. Every phase is bounded;
missing markers or notification overflow fail visibly rather than fabricate success.
The oracle is a **strong filesystem lifecycle proxy**, independently checked with
world/death screenshots. It is not an exact first-visible/controllable-frame oracle.
Graphics/explosion screenshot scores do not drive the loop or filter the symptom.

Operator setup once: prepare the intended Bloodborne 1.09 Central Yharnam save
and facing. The existing profile already supplies the working checkpoint.
The runner makes an independent profile copy via XDG_DATA_HOME and records the
actual save bundle opened by the game; on this host it is
`home/1000/savedata/CUSA00207/SPRJ0005`, despite app ID CUSA03173. It never resets,
writes or replaces the original profile/save. Repeated launches copy the same
seed. Supply another prepared shadPS4 profile with `--profile-template` if needed.
Keyboard mapping must retain Cross=n and LeftY plus=s, as in the verified config.

The old Enter + continuous `s` hold is retained only as `--x11-hold --death-hold`.
It is a diagnostic fallback and cannot be called a completed lifecycle campaign.
`--seconds` is an overall emulator budget; `--iterations` counts confirmed cycles;
stage/iteration timeouts bound progress. Finishing N cycles stops early. Timers
settle observed boundaries and control watchdogs; elapsed time never marks death.

## Future full Disabled / Precise captures (not run during implementation)

Use an active X11 display and XAUTHORITY, or a private Xvfb server as in the
validation note. The unchanged E0 producer generates substantial data in actual
3D gameplay: roughly 5–6 GB for two cycles here. Disk output lost records; bounded
RAM-backed output achieved zero drops. Every capture is checked, and any drops
cause failure even when lifecycle completion succeeded. RAM output is not a
universal guarantee; do not accept lossy data as exact evidence.

The following bounded four-iteration commands fit the demonstrated host's 16 GiB
/dev/shm more comfortably than a long campaign. Confirm available RAM and tmpfs
space before increasing N. Stage out and remove only the successfully retained
first RAM output before starting the second. Do not delete historical evidence.

```bash
SHADPS4_UMA_E0_MAX_BYTES=12884901888 python3 /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-deathloop/tools/uma_e0/run_capture.py --mode Disabled --death-loop --iterations 4 --seconds 300 --stage-timeout 110 --iteration-timeout 110 --display "$DISPLAY" --xauthority "$XAUTHORITY" --output /dev/shm/uma-e0-full-disabled
```

```bash
SHADPS4_UMA_E0_MAX_BYTES=12884901888 python3 /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-deathloop/tools/uma_e0/run_capture.py --mode Precise --death-loop --iterations 4 --seconds 300 --stage-timeout 110 --iteration-timeout 110 --display "$DISPLAY" --xauthority "$XAUTHORITY" --output /dev/shm/uma-e0-full-precise
```

Only the readbacks mode differs. Profiles, GPU settings, input plan and seed save
hashes must match. Logging duplicate suppression is disabled in the copied profile
so the two required PlayGo calls remain observable; it is recorded in launch data.
Upstream IPC disables automatic XML patches, and this runner sends no PATCH_MEMORY
commands. Current profile has none. Save/keys/profile copies stay private.

## Artifacts and offline analysis

Return these allowlisted artifacts from both outputs, **excluding profile-xdg**:

* `launch.json`: binary/E0 base/harness/component/input-plan/seed/actual-save hashes,
  requested/completed iterations, bounds, stall/failure and emulator/device-loss status.
* `lifecycle.jsonl`, `lifecycle-summary.json`, `host-events.jsonl`, `input-events.jsonl`.
* `iteration-*.png`, optional `screen-*.png`, and private `emulator-private.log`.
* `capture/metadata.json`, `events.bin`, `transport-check.json`.
* Semantic `summary.json`/`summary.txt` when generated with the unchanged analyzer.

Do not archive whole output directories indiscriminately: `profile-xdg` contains
copied private saves, keys and account state. Preserve raw data when staging out.
The structural transport check verifies every sequence with bounded memory and
rejects dropped, truncated or unclean captures. Full E0 semantic analysis is
unchanged, but deferred for large campaigns unless `--analyze` is specified:

```bash
python3 /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-deathloop/tools/uma_e0/analyze.py /path/to/retained/capture
```

Transport remains opt-in through SHADPS4_UMA_E0_CAPTURE; format, event meanings,
signal safety, scheduler observation, guest synchronization, BufferCache and
MemoryTracker are unchanged. A larger byte budget changes only the existing file
budget; no emitter sampling or event suppression was added.

## Events and trustworthy facts

Every little-endian record is thirteen uint64 fields:
`seq, ns, thread, kind_flags, context, session, tick, address, size, a, b, c, cmd_seq`.
`ns` is CLOCK_MONOTONIC nanoseconds. `kind_flags` low 32 bits are the enum in
`src/common/uma_census.h`; high 32 bits are event-specific flags. Thread is a
capture-local host TID (never a guest identity). Ranges are half-open. `context`
is a unique scheduler ID for GPU events and a raw mapping epoch for Map/Piece.
No raw host pointers or guest bytes are captured.

| Kind | Range / auxiliary fields | Meaning and limit |
| --- | --- | --- |
| Map | raw VA call; a=prot, b=map flags plus no-coalesce at bit32, c=name hash; flags=VMA type | accepted raw mapping before final coalescing; not one future import |
| Piece | clipped VA piece; a=PA, b=memory class, c=physical type | existing VMM backing metadata; physical allocation reuse distinguished by mapping epoch |
| Unmap / UnmapCall | affected subrange / successful API range; c=100 replacement,101 decommit | distinguish raw API calls and subrange removal/replacement |
| Protect / Name / MemoryType | affected range; a=new value | metadata changes only, no new transitions introduced |
| Vma | post-coalescing VA region; a=prot,b=piece count,c=no-coalesce | actual merged VMA sample; sampling weights repeated regions |
| GpuMap / GpuUnmap | actual renderer registration request | GPU eligibility uses upstream 40-bit address bound, not GPU protection bits |
| BlockSize | size=runtime sparse/BDA block size | device-selected value; never hardcoded universally |
| Buffer | guest range;a=read1/read-write3,b=texel,c=arena0/stream1/staging2 | ObtainBuffer intent candidate, not exact shader execution |
| Snapshot | guest source;a=stream1/image staging2/arena upload3 | record-time snapshot; later CPU writes do not modify it |
| CommandBegin/End | a=operation family on begin; cmd_seq unique | outer scope includes buffer prerequisites; failed/HLE scopes can have no actual consumer |
| Command | a=draw1/indirect2/dispatch3/runtime copy10/fill11 | actual command recording latches session and candidate tick; runtime copies retain outer cmd_seq |
| Session / SubmitSession | session ID; SubmitSession.tick=actual NextTick,a=has upload buffer | explicit recording-session membership in submission |
| Submit | size=command-buffer count,a=VkResult,b=submit-call ns,c=queue family | only success authorizes completion observer; event ns is after queue submit returns |
| Complete | actual submitted tick;a=wait result | direct existing-semaphore wait; ns is completion-observed upper bound |
| ArenaBind | a=memory_semaphore signal,b=bind result,c=owning graphics submit tick | separate sparse-memory timeline tied to exact scheduler context |
| Packet | guest destination;a=PM4 opcode,b=controls,c=packet ID | parse before OnFence and SignalFence; prospective session/tick |
| CpuWrite / Irq | store range or IRQ;a=origin opcode,b=source/pipe,c=packet ID if applicable | actual CP/CPU fast-path store or IRQ site, before existing action |
| Fault | bounded fault range;a=write1/read0,b=UFFD1/signals0 | access fault fact; range is not decoded instruction width |
| Watch | page range;a=total write watchers,b=read watchers,c=operation masks;flags=generic1/BufferCache region2 | existing watcher transition under its existing lock; attribution needs complete history |
| Tracker | accepted BufferCache invalidation range;a=1 | paired fault is a strong provenance proxy, not first-write-after-upload proof |
| Readback | download window;a=buffer2/image3,b=buffer download bytes | immediately before existing Finish; no new Finish |
| Finish | a=caller,b=begin0/end1,c=completed presubmit tick at end | existing wait fact; caller1=rasterizer,2=buffer download,3=sync image,4/5/6=presenter drain,7=EOS GdsStore |
| BackingWrite | originating VA;a=PA,b=origin tag,c=packet ID;flags=VMA type | directly before TryWriteBacking memcpy; bypasses VA faults and affects physical aliases |
| Resident | mirror allocated VA range;a=1 | actual BufferCache hold, not exact shader use |
| End | scheduler context;size=unobserved queued ticks, or context0 for collector | clean lifecycle marker; pending ticks remain unknown |

## Joins, grades and safety

Buffer prerequisites inherit outer `cmd_seq`; actual draw/dispatch recording
latches its current session/tick. Join `(scheduler,cmd_seq)` to that consumer,
then `(scheduler,session)` to explicit SubmitSession and successful Submit.
ObtainBuffer's own CurrentTick is never promoted to an actual submit tick. Arena-upload snapshots join their own recording session. Stream/image staging
snapshots use the enclosing recorded consumer join, while preserving their source
recording session in raw events. Image preparation within an outer draw remains
a conservative association if an internal consumer crosses sessions. Scheduler IDs separate draw, present and flip timelines; Presenter work
on draw_scheduler remains in the same context. BindSparse signals use a separate
memory timeline keyed by that BufferCache's draw scheduler context.

The observer waits directly on the existing work semaphore with 5 ms timeouts,
only for successfully submitted ticks. It never calls Scheduler::Wait, Flush or
Finish, never acquires a scheduler lock and adds no device ordering. It stops and
joins on scheduler destruction and quick_exit, before collector teardown. Unseen
queued ticks at stop are emitted, not inferred complete.

Fault/watcher producers use static POD storage, bounded CAS attempts, lock-free
64-bit atomics, clock_gettime and gettid syscall; errno is preserved. They add no
allocation, file I/O, formatter, ordinary lock, protection or watcher. Watcher
state is observed only inside locks already held by upstream. Raw faults never
inspect mutable scheduler or PageState containers. The fixed 65,536-record MPSC
queue drains on the collector thread. Queue/contention/file-budget failures count
as drops. Any drops invalidate exact/negative claims; raw observations remain.

The analyzer computes raw and merged candidate sizes, lifetimes (right-censored
at capture end), alias/fragmentation and boundary distributions, GPU-touched
candidate counts/peaks, crossings, churn and two hypothetical import policies.
Partial unmap invalidates a whole raw-import candidate; recreation occurs on next
use. Merged candidates follow existing VMA compatibility, but remain estimates:
actual import alignment/host allocation and old SDK coalescing can differ. The
80–120 us reference band is for complete 64-KiB object lifecycles only. Estimates
are reference-equivalent bands, not per-VMM-event production predictions.

Range/timeline overlap is an **upper bound**, not proof of a WAR/RAW failure.
GPU execution ranges and exact completion instants are not observed. RAW absence
under Disabled, UFFD read coverage, direct alias-VA CPU writes and exact BDA shader
access are **unobservable** with these hooks. Watcher attribution is a **strong
proxy** only with complete history or same-thread accepted-tracker pairing.
Event transport/mapping metadata/session membership are facts; no gameplay
correctness claim is promoted. Fence signals preceded by an existing Finish are
reported separately from signals before host submit (confirmed for that tick)
and signals before observed completion (upper bound). Mirror-hold backing
write overlaps are **possible existing mirror staleness**, not a stale-byte oracle.

## Validation and remaining scope

See `docs/research/uma-e0-validation.md` for original E0 producer validation and
`docs/research/uma-deathloop-validation.md` for the restored harness, V1–V4 results,
transport negative results and retained artifact paths.
Full death/reload captures are operator work; they were not run as a campaign.
