# E1A — guest completion on submitted host prefixes

Design/test scaffolding only. Exact E0 base:
`2437a5538bbc4fdffed8cf04bdb06ba08f91e095`; upstream below it:
`f73709cada28e77523e37806fee5c454711d24ba`. No production changes, new Finish,
barriers, guest signaling, shared buffers, game run or harness changes in E1A.

## Current bug and E0 evidence

Liverpool invokes EOP/EOS/ReleaseMem side effects while parsing, although prior
host command buffers can still be recording. The helper's store-before-IRQ order
is usually correct locally, but neither action has a host completion dependency.
Host completion and guest-memory visibility are separate obligations: current
mirrors/snapshots can hide timing mistakes that future shared backing would expose.

Read [E0 design](uma-e0-design.md) and [validation](uma-e0-validation.md). The small
existing `uma-e0-verified-disabled` / `-precise` summaries at this exact source have
zero drops and respectively 10,724 / 10,701 signals before their associated submit,
1,772 / 2,275 successful submits and matching completion observations. Those
associated ticks cannot have completed before their submit. This does not prove
which arbitrary packet's hardware scope those candidate joins represent.
Command IDs latch the real consumer recording session; explicit SubmitSession
membership joins to actual NextTick signals, keyed by scheduler instance.
ObtainBuffer.CurrentTick and parse-time CurrentTick remain prospective.
Completion-observed timestamps are upper bounds, not exact completion instants;
absence of an observation does not prove pending work. Precise recorded 504
existing buffer-download Finish calls. Synchronous image/EOS-GDS exceptions are
source facts; their absence in that short run is not negative evidence.

## Exact source path matrix

Links refer to files at this base; evidence below is static unless labeled runtime.
The physical renderer serializes graphics and compute onto its draw scheduler's
host graphics queue. An entire submitted prefix is a conservative completion
scope; it may include interleaved work from other guest rings and stage work that
EOS would not require on hardware. Do not claim exact stage/ring minimality.

| Packet/path | Guest side effect and current time | Prior work / current session | E1 treatment |
| --- | --- | --- | --- |
| [Gfx EventWriteEop](../../src/video_core/amdgpu/liverpool.cpp), [SignalFence](../../src/video_core/amdgpu/pm4_cmds.h) | OnFence, then 32/64-bit immediate/clock/counter store through TryWriteBacking; GfxEop IRQ inline after store, or IRQ-only | Completion-selected prior graphics work; current session can be unsubmitted | Seal relevant prefix; publish store then IRQ only after exact submitted tick. No-data/no-IRQ is not a visible signal. Preserve selectors; unsupported combinations remain explicit. |
| EventWriteEos SignalFence | OnFence then 32-bit TryWriteBacking during parse; no direct IRQ in helper | Prior selected shader event; current session can be unsubmitted | Same ticket mechanism, conservative whole-prefix scope initially. Event type/stage must be decoded/documented; do not relabel EOS as universally full-pipeline EOP. |
| EventWriteEos GdsStore | Helper does nothing; parser asserts one DWORD, existing Finish(7), CPU ReadDataFromGds, direct guest store | GDS producer must finish; existing global wait already covers it | Retain synchronous path in E1B; return its completed-prefix proof. Do not add a second submit/GPU wait. Async conversion later needs a prefix-owned snapshot, not late live-GDS read. |
| Compute ReleaseMem scalar | OnFence, direct VA store for 32/64/clock/counter, then compute-pipe IRQ inline | Selected release event/cache scope; compute work can be unsubmitted | Delay descriptor execution behind submitted prefix. Unlike EOP it uses direct VA stores, so fault/readback behavior and memory lifetime need a checked CP-owned publication operation. |
| ReleaseMem GdsMemStore | OnFence; CopyBuffer(GDS→guest mirror) records GPU transfer; helper immediately signals IRQ | Prior GDS producers **and this transfer**, with guest visibility not guaranteed by mirror copy | Seal after recording required transfer/snapshot. IRQ follows completion **and guest publication**. A late arena download can read later writes; preserve a private snapshot/version. This is a distinct integration step, not a scalar callback. |
| WriteData, gfx and compute | OnFence then immediate memcpy; dst_sel 2/5 accepted, wr_one_addr unsupported | CP write; wr_confirm concerns the write, not proof of all prior shader completion | Do not delay every WriteData behind a global GPU fence. Preserve CP order; honor explicit waits/engine semantics. Device-mirror hazards are not solved merely by E1 completion callbacks. |
| DmaData CPU fill/copy fast paths | Rasterizer FillBuffer/CopyBuffer immediately fill/memcpy if tracker/image conditions permit | CP DMA operation; no host GPU command in fast path; prior device use may be masked by tracker decisions | Keep decisions/timing unchanged in E1A. cp_sync/raw_wait/engine fields require separate packet-specific treatment, not blanket completion signaling. |
| DmaData GPU/GDS paths | Fill/copy recorded, not completed; both gfx/compute; compute special indirect-dispatch rewrite records a patch association | DMA producer/consumer dependencies; current session can be unsubmitted | A subsequent completion fence must include the transfer. cp_sync/raw_wait currently not enforced by these branches; a required CP-DMA wait must yield the relevant guest ring, not globally Finish. Separate follow-up admission if not needed by scalar E1B. |
| WaitRegMem | CPU register/guest-memory compare; gfx/compute coroutine yields until true; gfx VO optimization blocks on vo_cv | Predicate/explicit engine wait; may consume a deferred completion label. CPU memory compare is not an automatic GPU readback | Ensure fence submission before waiting. CP must service ready actions while waiting; worker must not rely on later scheduler Pop. Preserve VO progress. General GPU-produced guest data under Disabled readbacks remains a visibility gap, not solved by a tick alone. |
| MemSemaphore | Signal increment/write=1 during parse; wait polls >0/yields then CPU decrement | CP semaphore semantics/client_code; not automatically shader-completion semantics | Do not indiscriminately convert to EOP. Preserve signal/consume ordering and service pending completions. client/mailbox/concurrency and broader memory visibility remain separately reviewed. |
| OnFence | ProcessDownloadImages only; modified sync image download records copy, existing Finish(3), invalidate, TryWriteBacking | May complete the entire prior prefix synchronously; otherwise **does not submit/synchronize BufferCache** | Propagate an explicit completed-prefix receipt from the existing sync branch. Presence of OnFence alone is not proof of completion. |
| Buffer download/readback | Existing Finish(2), staging invalidate, backing writes | Required download completion, not a new E1 observation wait | Retain; do not double-delay or use it as a universal substitute for completion signaling. |
| IRQ dispatch | IrqC::Signal invokes handlers inline under per-IRQ lock; GfxEop/compute events notify equeues | Associated store precedes IRQ today, but both are early | One ordered publication action: checked store, then IRQ. No success IRQ after failed/cancelled store. Preserve execution affinity/reentrancy; never hold scheduler/job locks across handler execution. |
| Patched NOP GfxFlip / GpuIdle | GfxFlip parse convenience triggers VideoOut handler; GpuIdle follows CP submission processing/flush | GfxFlip handler can record/submit presenter work; GpuIdle is current CP admission bookkeeping, not host GPU-idle proof | Do not fold these into scalar EOP without a distinct contract review. Delayed GfxFlip cannot be moved to a worker casually. |

Sources additionally inspected: [Rasterizer transfers/fences](../../src/video_core/renderer_vulkan/vk_rasterizer.cpp),
[texture downloads](../../src/video_core/texture_cache/texture_cache.cpp),
[backing writes](../../src/core/memory.cpp), [IRQ controller](../../src/core/platform.h),
[VideoOut driver](../../src/core/libraries/videoout/driver.cpp),
[VideoOut waits](../../src/core/libraries/videoout/driver.h) and
[Gnm event handlers](../../src/core/libraries/gnmdriver/gnmdriver.cpp).

External primary-source cross-check: [AMD PAL CmdUtil](https://github.com/GPUOpen-Drivers/pal/blob/dev/src/core/hw/gfxip/gfx9/gfx9CmdUtil.cpp)
builds WRITE_DATA with its own write-confirm/engine controls and a distinct
CP-DMA sync wait; [Linux CIK fence emitters](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/amd/amdgpu/gfx_v7_0.c)
use EOP/ReleaseMem for fences. These support separating packet families, not an
exact PS4 hardware/cache-stage specification. Unknown selector semantics must
remain an explicit E1B acceptance boundary.

## Scheduler timeline and minimal proposed APIs

[Scheduler](../../src/video_core/renderer_vulkan/vk_scheduler.cpp) allocates
`signal_value=work_semaphore.NextTick()` only in SubmitExecution, under static
submit_mutex. CurrentTick starts1; NextTick returns T and increments to T+1
**before** on_submit/EndSession. Submit gathers all accumulated upload/primary
sessions, signals T, clears sessions, then BeginSession starts the suffix.
Rasterizer's on_submit flushes existing barriers and sparse arena binds and adds
their memory_semaphore dependency. Keep that path; its tick is not the work tick.
Only successful queue submit can create a completion ticket. Existing Flush void
and Rasterizer's pre-read CurrentTick are not a sufficient typed success receipt.

Proposed names/contracts, **not implemented**:

```cpp
struct SubmittedTick { SchedulerIdentity scheduler; u64 tick; };
Expected<SubmittedTick, SubmitError> FlushAndGetSubmittedTickForCurrentPrefix();
Expected<SubmittedTick, SubmitError> FinishAndGetCompletedPrefix(); // existing waits only
void EnqueueGuestSignal(SubmittedTick, OwnedGuestSignal, GuestQueueSequence);
```

Flush's internal SubmitExecution returns the exact successful signal_value,
including finalization/upload/sparse-bind dependencies. Invocation is CP-owned
at the packet boundary, after existing OnFence and any fence-owned transfer
preparation; packet parsing must not resume first. This seals A/fence separately
from B: A and earlier accumulated sessions are submitted under T; B records into
the newly started session under a later ticket. A completion may publish while B
is still incomplete. Mere BeginSession does not force submission/progress.
If there is no work, a cheap empty prefix submit is a valid conservative first
implementation; never turn that into a global completion wait.

For existing synchronous OnFence/EOS-GDS, propagate the exact successful Finish
receipt through DownloadImageMemory→ProcessDownloadImages→OnFence. A completed
receipt covers only the prefix already consumed; if the packet then records GDS
transfer work, it must seal that new work separately. Do not infer emptiness from
CurrentTick, invent broad recorder instrumentation or weaken an existing Finish.
Completed receipts still enter the ordered publication lane; only an empty lane
can execute immediately on CP. GPU-complete B must not overtake pending A merely
because its OnFence happened to Finish.

## Completion execution and why current deferral is insufficient

DeferOperation captures prospective CurrentTick and executes only during later
submit/PopPendingOperations, under pending_ops_mutex. It can starve when CP is idle
or waiting, and its callbacks may record commands (FaultManager). Do not use it.

DeferPriorityOperation wakes a FIFO worker and waits work_semaphore.Wait, outside
its queue lock. This is useful precedent for independent progress, but **not
sufficient as-is**: it captures CurrentTick, has no exact successful-ticket input,
no owned guest action, and Semaphore::Wait uses infinite timeout/retry without
stop/device-loss outcomes. Calling it after Flush captures T+1 and can wait forever.
Enqueueing before Flush still does not isolate/prove the prefix or submit success.
An unrelated FIFO resource callback waiting on unsubmitted T+1 can also block a
late inserted T action; shared priority queue ordering cannot be assumed safe.

Smallest safe design: a guest-completion lane owned by the renderer/CP lifecycle,
accepting only successful submitted tickets (and optional completed proofs). Its
worker uses bounded, cancellable direct waits on the existing timeline; it never
Flushes, Finishes, records commands or invokes IRQ handlers. It posts owned ready
actions to the CP via its existing command-message wake mechanism, independent
of future PM4 submissions or scheduler polling. Reuse common wait machinery where
practical, but do not enlarge/refactor every resource callback in E1B. Pre-fence
asynchronous readback CPU publications, if required by a signal, need an explicit
visibility dependency/ack; GPU completion alone does not prove those copies done.

PatchFlipRequest deserves a regression boundary: it emits a plain WriteData VO
lock before PatchedFlip, whose handler asserts that lock, and optionally emits a
later user EOP label/IRQ. Delaying all WriteData or moving that NOP handler to a
worker would break this protocol. Keep the lock/NOP path and include its presenter
work in the later EOP prefix; an empty new recording session does not erase prior
submitted draw work. E0 candidate-tick evidence alone cannot identify that scope.

The CP applies a per-guest-queue sequence gate: store/clock/counter action then its
IRQ, once, FIFO for comparable completion packets. The single physical timeline
can conservatively overorder interleaved rings; it must not invent a guest-wide
order absent from packet semantics. Plain CP writes are not blanket fenced behind
every EOP: their ordering follows actual engine/explicit-wait requirements.

## Deadlocks, ownership, shutdown and errors

* Copy descriptors/data/VA/selector/pipe ID at parse time. Never retain PM4 headers,
  copied command-buffer spans, local lambda references, mutable GDS pointers or
  unowned `this` in a delayed action. Immediate data stays captured; clock/counter
  values are sampled at publication as current approximations, not claimed exact
  physical GPU timestamps.
* Worker only posts ready work; CP applies guest stores and IRQs. Current Gnm
  completion handlers notify equeues; separate GfxFlip handlers can record/submit
  presenter work inline. Preserve CP affinity rather than assume arbitrary IRQ
  callbacks remain worker-safe. ReleaseMem direct-VA faults can themselves require
  SendCommand<true>; executing them on the waiting worker creates another cycle.
* Liverpool's ProcessCommands is serviced during active/yielded work and wakes
  on num_commands when idle. However WaitVoLabel's blocking optimization waits
  only on vo_cv and does not service command messages. E1B must make that path
  cooperative or message-aware (with callbacks run after releasing VO mutex).
  Simply enqueueing CP-ready signals is insufficient there. No callback may wait
  synchronously for its own CP dispatcher while that dispatcher is parked.
* Scoped owner generation/stop token covers wait jobs and already queued CP jobs.
  Stop admission, cancel unpublished descriptors, join the finite-wait worker,
  invalidate/drain queued ready work **before** CP/Memory/IRQ targets are destroyed.
  Add an explicit quick_exit shutdown hook; existing Emulator::Shutdown does not
  tear down all renderer/CP singletons. Presenter teardown's Finish calls are not
  an E1 progress or cancellation plan. Existing priority worker can hang in its
  current infinite wait, so that lifetime cannot be borrowed without changes.
* Failed submit produces no ticket; device lost/other wait failure cancels success
  publication and reports the emulator error. Never spin forever or synthesize a
  completed timeline/store/IRQ to make progress. Cancelling on shutdown is not
  guest-success completion.
* Destination uses a checked memory-manager operation on CP while its owner is
  alive, not a retained host pointer. Preserve legal label storage and validate
  invalid destinations without false IRQ. Legal guest allocation lifetime until
  completion remains a precondition. Remap/physical-reuse ABA detection is **not**
  solved here: it needs a separately admitted VMM generation/lease seam; do not
  claim numeric VA or TryWriteBacking alone pins the original mapping or start E2.
* Release-GDS asynchronous publication needs a private source/result snapshot in
  the sealed prefix. Later GDS/arena writes can run before a CPU completion handler
  reads live memory. Retain existing EOS Finish instead of pretending tick wait
  solves that version race. Packet-owned GDS transfer and CPU-visible snapshot
  publication must precede its IRQ; unsupported cases cannot be silently scalarized.

## Regression scaffolding and E1B sequence

`tools/e1a_tests/packet_probe.cpp` compiles real PM4 SignalFence helpers against
owned CPU memory. Test-only assertions/clock declarations avoid linking the
emulator; bit fields, selectors and packet bodies are real. Five desired tests
are expected failures at this base: EOP/EOS/Release scalar early stores, IRQ-only
early signaling, and Release-GDS IRQ after mere transfer recording. Positive
controls establish current store-before-IRQ and EOS-GDS helper no-store behavior.
These are source characterizations: E1B likely keeps helpers synchronous but
invokes them later. Therefore move the failing boundary assertions onto the real
new adapter; do not pretend unchanged helper tests automatically turn green.

Fourteen deterministic fake-scheduler tests encode store/IRQ completion, prefix
separation, idle-CP progress, multiple-FIFO ordering, completed-Finish reuse,
wrong/unsubmitted-ticket rejection, submit/device loss, pending/ready cancellation,
owned payload, wait-label progress, blocked-VO obligation and GDS snapshot version.
They are scaffolding, not a production integration or Vulkan/lifetime proof.
Full-CP/GDS snapshot/quick_exit integration fixtures are specified here rather
than inventing invasive runtime seams in E1A.

E1B implementation sequence (bounded slices):

1. Make internal SubmitExecution return a typed successful ticket and expose
   FlushAndGetSubmittedTickForCurrentPrefix; prove A/fence/B against the real
   scheduler with a fake queue. Preserve existing callers and sparse waits.
2. Add owned, cancellable known-tick completion lane and ready-CP delivery, per-ring
   sequence gate and explicit quick_exit/destruction ordering; test no-later-submit,
   pending/ready cancellation and failure. Make VO waits message-aware/cooperative.
3. Admit scalar EOP/EOS/Release selectors individually. Capture owned packet data,
   propagate existing completed-Finish receipts and publish store then IRQ on CP.
   Run boundary regressions against this real adapter, not only the reference model.
4. Separately integrate Release-GDS transfer snapshot/guest visibility before IRQ;
   retain synchronous EOS-GDS. Audit WriteData/DmaData cp_sync/raw_wait and
   MemSemaphore/WaitRegMem dependencies as distinct contracts; avoid blanket stalls.

Acceptance: real-adapter tests prove all seven requested invariants, submitted
success identity/prefix scope, relevant store visibility and error/cancel outcomes;
no new global Finish, no implicit worker submit, no later-command dependency,
no premature/double IRQ, no idle/VO-wait deadlock or callback after target teardown.
Existing synchronous exceptions stay synchronous without duplicate GPU wait.
Unsupported selector/stage/mapping-lifetime boundaries remain explicit and block
claims beyond validated scope. A later short target validation must be separately
authorized; it was not run here. E2/shared imports and broad RAW/WAR/backend fixes
remain excluded. E1B scalar/progress/lifetime slice is **medium**; folding every
GDS/DMA/visibility contract into one patch would become large and is not advised.
