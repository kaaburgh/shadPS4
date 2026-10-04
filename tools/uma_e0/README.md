# Observation-only UMA E0 census

Base upstream **f73709cada28e77523e37806fee5c454711d24ba**. Design was committed
first in **d8e993db**, with the verified command/Finish/backing-write corrections
in **76a61987**. No imports, synchronization changes, new protection/watchers,
barriers, Finish calls, upload/readback decisions or shader translation changes.

## Capture

The recorder is opt-in: `SHADPS4_UMA_E0_CAPTURE=/absolute/new/directory`.
Directory creation is exclusive; existing captures are never overwritten.
`SHADPS4_UMA_E0_MAX_BYTES` optionally sets the events.bin budget (default 512 MiB).
At the budget limit events are dropped and counted, not silently rotated.
The writer updates metadata every 250 ms and at normal/quick shutdown. A killed
process can leave incomplete metadata/tail; analyzer never promotes that result.

Use the existing verified disposable target, including its independent `app` and
`app-UPDATE` siblings and copy receipt. Existing source trees and the E0b archive
remain untouched. Raw logs/screenshots are private and must not be committed.

On the operator's active X11 display (historically :1; use its actual DISPLAY and
XAUTHORITY), run these two commands **sequentially**. Config override uses the
existing historical runner's TemporaryReadbacksOverride and restores exact bytes.
All other selected GPU settings, input configuration and save/cache state remain
the operator's existing state. Upstream IPC disables automatic XML memory patches;
this runner sends no PATCH_MEMORY commands. This is recorded explicitly. The
current profile has no patch XML files. If the intended scenario requires such a
patch, these commands do not establish that scenario until its patch input is
provided through the existing upstream IPC patch route. Each launch records their hashes;
compare launch.json before treating runs as comparable. Persistent caches can
warm and save state can drift; this is exploratory-unverified evidence.

```bash
SHADPS4_UMA_E0_MAX_BYTES=2147483648 python3 /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-e0/tools/uma_e0/run_capture.py --mode Disabled --seconds 300 --death-loop --death-hold 180 --screenshots --display "$DISPLAY" --xauthority "$XAUTHORITY" --output /media/ubuntu/UsbSSD447G/shadps4/runs/uma-e0-full-disabled
```

```bash
SHADPS4_UMA_E0_MAX_BYTES=2147483648 python3 /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-e0/tools/uma_e0/run_capture.py --mode Precise --seconds 300 --death-loop --death-hold 180 --screenshots --display "$DISPLAY" --xauthority "$XAUTHORITY" --output /media/ubuntu/UsbSSD447G/shadps4/runs/uma-e0-full-precise
```

Reach **Central Yharnam**, confirm the same checkpoint and facing/direction, then
press Enter in the capture terminal. This reuses the existing external X11 helper
holding `s` (configured LeftY plus = historical death direction 255) for 180 s.
Confirm actual key bindings in launch.json input hashes and actual repeated
fall/death/reload from screenshots; the key hold is **not a lifecycle oracle**.
The 300-second budget includes menu navigation and operator confirmation. If the
checkpoint cannot be reached in time, the run remains a startup/menu observation.
Both runs must use the same navigation/death route. The script bounds launch,
input hold and shutdown and uses upstream IPC STOP. It cannot inherit old
source-dependent SHADPS4_BB_DEATH_LOOP or .bbpad behavior on upstream.

The historical full benchmark is
`/home/ubuntu/bb-shadPS4-correctness-instrumentation/tools/run_bb_death_reload_benchmark.py`.
Only its independent config helper is imported. Reused external input/screenshot:
`/media/ubuntu/UsbSSD447G/shadps4/work/bb-1.09-exploration/diagnostics/x11_control.py`
with its existing `x11-venv/bin/python`. Previous instrumentation branches, patches
and source-dependent replay files are inventoried, not cherry-picked.

Artifacts in each output directory:

* `launch.json`: exact binary/source/harness/helper/config/patch/input/receipt
  identities, bounds, termination and operator-reported checkpoint.
* `capture/metadata.json`: producer/build/host/device/config/game/timestamps/drops.
* `capture/events.bin`: unchanged raw 104-byte records.
* `capture/summary.json` and `summary.txt`: aggregate distributions and limits.
* `screen-*.png` and `emulator-private.log`: private scenario/runtime checks.

Return **both complete output directories**, preferably separate tar.gz archives.
Raw events plus metadata and launch.json are needed to revise classifications.
Return screenshots for the route/death/reload oracle. Logs remain private.

Offline replay:

```bash
python3 /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-e0/tools/uma_e0/analyze.py /media/ubuntu/UsbSSD447G/shadps4/runs/uma-e0-full-disabled/capture
python3 /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-e0/tools/uma_e0/analyze.py /media/ubuntu/UsbSSD447G/shadps4/runs/uma-e0-full-precise/capture
```

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

See `docs/research/uma-e0-validation.md` for commands, private artifact paths,
upstream test failures, bounded runtime results and the remaining coverage limits.
Full death/reload captures are operator work; they were not run as a campaign.
