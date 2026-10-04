# E1B scalar completion implementation and bounded validation

Branch: `research/uma-e1b-scalar-completion`, fork kaaburgh/shadPS4.
Exact E1A base: `8e2a780767ce26840ba73d9d7bb0835f85e91436`.
E0 beneath it: `2437a5538bbc4fdffed8cf04bdb06ba08f91e095`;
upstream beneath E0: `f73709cada28e77523e37806fee5c454711d24ba`.
The [E1A contract](uma-e1-design.md) was rechecked against this source; no material
mismatch required redesign. No E2/shared buffers or BufferCache policy changes.

## Submission and publication

`Scheduler::FlushAndGetSubmittedTickForCurrentPrefix()` returns
`std::expected<SubmittedTick, vk::Result>`. SubmittedTick has private construction,
a retained scheduler timeline identity, exact Value and monotonic SubmittedAt.
Only successful queue submit mints it. Failure gives no ticket and scalar admission
terminates through the existing fatal error path without a guest store/success IRQ.
The tick is the same work_semaphore.NextTick used in SubmitExecution, not CurrentTick.

The production SubmittedPrefix adapter detaches all accumulated upload/primary
sessions after normal on_submit/EndSession preparation. Existing barrier flushes,
sparse arena binds/memory_semaphore waits and timeline signals are retained.
Submission success starts a fresh suffix before parsing resumes. PopPendingOperations
can record into that suffix; its commands cannot enter the submitted vector.
Typed identity distinguishes schedulers; Presenter submits on the same draw scheduler.

An empty prefix still submits a normal finalized command buffer and gets a real
receipt. IsKnownEmptyPrefix is a conservative recorder-access proxy, not proof that
no timing command or sparse dependency exists. It is safe for reusing an explicit
Finish receipt only when no new suffix command-buffer access occurred.

Existing Finish returns a separately typed CompletedPrefix via
FinishAndGetCompletedPrefix. A successful existing wait creates that proof.
DownloadImageMemory(sync) -> ProcessDownloadImages -> OnFence propagates it;
OnFence without a download supplies no proof. No Finish was added.

GuestCompletionLane accepts only owner-matching submitted tickets or CompletedPrefix.
Its dedicated jthread uses Scheduler::WaitSubmitted: direct vkWaitSemaphores with
50-ms cancellation polling, no Scheduler::Wait/Flush, commands, queue submits,
IRQ or guest-memory access. Timeout means retry, not a semantic GPU timeout.
Wait failure closes admission, cancels unpublished jobs and posts a CP error.
An earlier completed proof must not hide a later terminal wait error (regression).
The legacy general deferred workers are untouched and unused by this lane.

Owned ScalarCompletion copies selector, VA, immediate payload, event/stage flags,
IRQ/pipe, logical guest queue, packet ID and timestamps. Clock/counter values are
sampled on CP publication using the existing host approximations, not a claimed
physical GPU timestamp. No PM4 span, stack reference, GDS pointer or host destination
pointer is retained.

CpCompletionTarget posts owned messages through Liverpool::SendCommand. Worker
posts only; CP DrainCompletions does the checked store and then the IRQ. Per-queue
issued/published sequence gates retain order, including a completed proof arriving
before an older async action. One physical timeline conservatively orders host
execution; publication does not impose a guest-global sequence across queues.
FIFO waiting is safe here because all asynchronous receipts come from one timeline
in submission order; synchronous proofs enter ready gates directly.

Idle CP wakes through the existing num_commands/submit_cv path. Ordinary WaitRegMem
coroutine yields service that same queue. VO blocking uses WaitWithProgress: finite
5-ms sleeps, command notification/predicate checks, and CP pumping only after the VO
mutex is released. No callbacks execute under VO, scheduler or lane locks.

## Packet/path matrix

| Path | E1B behavior | Prior-work scope / remaining boundary |
|---|---|---|
| EOP None | IRQ-only or supported IRQ-confirm combination waits for prefix; no store | Entire recorded prefix; invalid selector/IRQ combinations explicit |
| EOP Data32Low / Data64 | Owned 4/8-byte backing store, then optional GfxEop IRQ | Exact accepted prefix T |
| EOP GpuClock64 / PerfCounter | CP samples existing approximation after T, stores8, optional IRQ | Not exact GPU timestamps |
| EOS SignalFence=2 | Owned32-bit backing store after T; no new IRQ | Conservative prefix scope, stage/event metadata copied |
| EOS GdsStore=1 | Existing synchronous Finish(7), live GDS read/store retained | CompleteThrough(proof) drains older scalar publications before the inline GDS store; not converted async |
| ReleaseMem Data32Low / Data64 / GpuClock64 / PerfCounter | Checked direct-VA CP store after T, then existing compute pipe IRQ for supported selectors | None, IrqUndocumented, IrqWhenWriteConfirm IRQ semantics retained; None data/GDS excluded from scalar admission |
| ReleaseMem GdsMemStore | Original GPU copy and original early IRQ retained | Explicit E1C gap; needs private prefix-owned snapshot/visibility integration |
| WriteData gfx/compute | Existing OnFence where present, then immediate CP memcpy | Never routed through lane; VO/GfxFlip lock protocol retained |
| DmaData CPU fast paths | Existing fill/memcpy decisions/timing retained | cp_sync/raw_wait audit separate |
| DmaData GPU transfers/GDS | Existing recorded fill/copy | Subsequent scalar packet seals/includes those recorded commands |
| WaitRegMem | Original register/memory predicate and coroutine yields | E1-ready labels can publish; general GPU-written RAW under Disabled readbacks unresolved |
| MemSemaphore | Original CP signal/wait/consume behavior | No blanket completion conversion |
| OnFence/readbacks | Existing sync image Finish(3), buffer Finish(2) retained | No inference OnFence alone means complete; no copy/coherence-policy change |
| GfxFlip / GpuIdle IRQ paths | Unchanged | Not admitted scalar GPU-completion semantics |

EOP/EOS retain physical backing write semantics. TryWriteCompletion validates the
whole 4/8-byte physical destination before storing (including segment crossings).
ReleaseMem retains direct-VA CPU tracking/fault semantics. The checked operation
uses a try-lock to serialize VMM mutation, validates mapped CPU-writable destination, releases the
VMA read lock before memcpy so existing fault/readback handling can query backing,
and keeps the mutation guard until the store finishes. If busy, CP services existing
messages and sleeps with a bounded command-aware wait before retrying. Reentrant
completion drains are guarded so per-queue store/IRQ order is preserved. This avoids
a cycle with a VMM thread holding the mapping lock while awaiting an existing CP
readback. No mapping/owner lock is held while servicing messages. A focused actual
retry-adapter regression exercises that cycle and cancellation. Unsupported/unmapped
publication fails explicitly and emits no success IRQ.

Numeric VA is not a pin/lease. Admission requires the guest to retain the destination
allocation until completion publication. General remap/ABA/generation leases are
not solved; CPU-read-only/GPU-only direct scalar destinations are an explicit
unsupported boundary. Backing writes still bypass cache notifications: mirror
staleness, alias CPU-write coverage and GPU->CPU RAW/coherence are not fixed here.

## Lifetime and errors

Stop admission -> invalidate CP target token -> cancel lane jobs -> join finite-wait
worker -> request CP stop/wake -> join CP -> destroy dependent resources. call_once
serializes lane shutdown; join mutex serializes external CP join. No owner mutex is
held across publication/IRQ, and CP-side error shutdown avoids self-join. A callback
already executing on CP is protected by that join; queued unpublished messages
observe an invalid token. Presenter/Rasterizer destruction stops before dependent
resources; Emulator::Shutdown explicitly does so for quick_exit as well. No detached
worker retains emulator objects. A 65,536-job admission ceiling prevents unbounded
lane growth and fails explicitly if exceeded. Shutdown cancellation does not fake
stores/IRQs. The existing Finish point now uses direct accepted-ticket waits with an explicit
failure outcome and retains the known-GPU-tick refresh. Failed synchronous waits
cannot mint a CompletedPrefix or proceed to a scalar success action. General-defer
infinite waits remain legacy behavior; this lane never borrows them. Existing
synchronous Finish still waits for valid long-running GPU work; it is not converted
to an asynchronous operation or given a semantic GPU timeout. GPU device-loss was modeled, not
induced on the physical GPU.

## Regression and build evidence

[Tests/commands](../../tools/e1b_tests/README.md): production C++ detach/ticket,
packet descriptor, completion/gate, CP-delivery token and VO-progress adapters pass.
Idle mailbox test wakes a parked CP endpoint without any future submit. Shutdown
covers pending wait and ready-but-unpublished token, owned payload, failed submit,
wait failure, failure after a proof and successful store-before-IRQ. ASan+UBSan pass.
Three Python evidence/parser guards pass (including early-store/drop controls).
E1A now has **20 pass + one intentional Release-GDS XFAIL**, four scalar assertions
bound to production adapters. Unchanged E0 analyzer tests: **7 pass**.

Real device Scheduler test passed: upload+A submitted under T, B recorded into the
suffix, direct wait for T sees upload+A and B remains zero/unsubmitted; then B is
submitted/completed. Existing Finish proof admission performs zero additional waits.
Normal RelWithDebInfo clang19/Ninja emulator build passes. Existing upstream suite:
**380/423 pass, 43 fail**. Failures are all in the previous 44-failure GCN baseline;
GcnTest.min_nan passed this time. No shader/test implementation changed to fix those
failures. No existing scheduler renderer suite was available beyond the new adapter
and real-device checks.

## Short Bloodborne evidence (exploratory, not promotion)

RTX5070Ti, NVIDIA595.91.07 (Vulkan reports595.91.7.0), Linux7.0.0-38-generic,
Bloodborne1.09 disposable app+app-UPDATE, prepared private copied Central Yharnam save,
Disabled readbacks, signal tracking, no automatic XML/PATCH_MEMORY edits. Maintained
bb-deathloop1.0.0 was reused unchanged with explicit binary/worktree/target/profile.
Its compound filesystem+INFO lifecycle oracle is a strong proxy; screenshots show
Central Yharnam gameplay and loading, not an exact fall/death timestamp.

- Initial E0-off smoke: 2/2, exit0, no force/device loss; cycles36.282/37.184s.
- Repeat after CP target extraction: 2/2, exit0, no force/device loss;
  cycles37.713/36.190s. STOP safely cancelled three unpublished jobs; a further sealed
  prefix raced admission shutdown, explaining prefix/publication count difference.
- Final checked-store build22785b9a: 2/2, exit0, no force/device loss;
  cycles37.696/46.905s. New VMM-retry/cancellation tests and sanitizers pass.
- First E0 run: 2/2/exit0, but existing512-MiB cap yielded49,750,553 drops. Retained
  as negative transport evidence; not used for complete joins.
- Repeated same bounded E0 smoke with existing MAX_BYTES option raised to12GiB,
  RAM collector: 2/2/exit0, cycles36.426/42.582s, no force/device loss;
  **55,408,071 events, zero drops, clean transport**, copied/hash-verified to SSD.
  All **917,301** scalar actions (EOP910,992, EOS6,250, ReleaseMem59) joined accepted
  submissions and independent E0 completion records. Zero recorded scalar stores/IRQs
  preceded the E1 completion observation; zero per-queue/timestamp violations and no
  missing successful receipt. Existing Finish was not encountered in this scenario;
  synchronous proof/sequence exceptions are covered by unit/real-device tests.

E0 capture code/format and canonical analyzer meanings are unchanged. The targeted
E1 analyzer consumed the complete raw E0 stream and joined packet context + exact
accepted E1 ticket. **883,857** publications preceded the independent E0 observer's
later timestamp: that observer is an upper bound. E1's separate successful wait
already proved completion, so this is not an early-host-publication finding. No
parse-before-submit admitted scalar publication remains in this joined smoke.
The broad full VMM/hazard analyzer was stopped after about15minutes; it is unnecessary
for these scalar joins and has no completed new full-census report. Raw data remains
available for that separate analysis. No full Disabled/Precise campaign ran.

Runtime receipts: initial off7df40b2b, E0 zero-drop5a024c7e, repeated offa8f17421.
The f43cdbb6 change affects only terminal wait failure after an earlier proof;
its model/sanitizer regression and normal build pass. The checked-store lock-progress
fix22785b9a received the final two-cycle smoke above. The final synchronous Finish
error-proof change0ab8e838 passed failed-proof/foreign-proof controls, normal build
and the real Vulkan prefix/upload/Finish proof test again. Game smoke did not inject
a device loss; its normal scalar path is unchanged by that error handling. Documentation commits do not change production behavior. Private
launch manifests retain binary hashes/source identities; these are exploratory
inputs, not a strict promotion-grade build/content manifest.

## First-order churn / latency

Zero-drop E0 interval:917,301 E1 prefix submits;501,922 recorder-empty proxies
(**54.72%**);923,450 total draw-scheduler submits; depth high-water10; zero unpublished
cancellations. Actions8630.5/s over106.286s of publication observations. Total across
schedulers:8536.1 submits/s over the full E0 interval (different interval endpoints).
Historical E0 two-cycle zero-drop archive at2437a553 had draw6394/109.859s=58.20/s,
all schedulers9837/109.859s=89.54/s. Similar scene/profile/mode/hardware; historical
capture/tool timing and workload are not an exact controlled A/B. The increase is
large; game smoke alone does not establish acceptable production overhead. No
empty-prefix or batching optimization was attempted.

| Interval (ms) | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| parse -> submit |0.000942|0.001714|0.002555|2.663457|
| submit -> E1 completion observed |0.031692|0.262580|0.564107|66.201389|
| completion ready -> CP publication |0.002765|0.013356|0.023345|14.838843|

E0-off repeat: p50/p95 submit->observed0.031862/0.261829ms;
ready->publication0.002034/0.010931ms. Optional trace file I/O affects these timings;
no instrumentation-overhead or universal production latency claim is made.

## Artifact locations / reproduction

Private runs under `/media/ubuntu/UsbSSD447G/shadps4/runs/`:
`uma-e1b-prefix-selftest`, `uma-e1b-off`, `uma-e1b-final-off`, `uma-e1b-checked-off`,
`uma-e1b-disabled` (cap failure), `uma-e1b-disabled-zero-drops` (valid).
The latter contains capture/events.bin + metadata.json, launch.json,
validation-inputs.json, capture-retention.json, binary-wrapper.sh, lifecycle.jsonl,
lifecycle-summary.json, screenshots and private emulator log. Raw events:
5,762,439,384 bytes, SHA256
`1e68367eb90fce4f571142f17922979531743acabf4c55ad85cac13df8064bd2`.
E1 trace and timing JSON are sibling files `uma-e1b-*-trace.jsonl`/`*-timing.json`.
Return the compact timing JSON, launch/validation/lifecycle summaries and supporting
screenshots first; raw E0 and E1 traces remain local for a requested deeper join.
Do not commit/share unrestricted private logs or proprietary profile/game data.

Reproduce the bounded off check (same command for Precise by changing mode; no full
campaign is authorized by this note):

```bash
python3 /home/ubuntu/bb-shadPS4-correctness-instrumentation/tools/bb_deathloop/run.py \
  --binary /media/ubuntu/UsbSSD447G/shadps4/shadPS4-build-uma-e1b/shadps4 \
  --worktree /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-e1b \
  --target-root /media/ubuntu/UsbSSD447G/shadps4/games/CUSA03173-1.09-DISPOSABLE \
  --profile-template /home/ubuntu/.local/share/shadPS4 \
  --x11-python /media/ubuntu/UsbSSD447G/shadps4/work/bb-1.09-exploration/diagnostics/x11-venv/bin/python \
  --display :99 --mode Disabled --iterations 2 --seconds 240 \
  --stage-timeout 110 --iteration-timeout 110 --output /absolute/new/smoke-output
```

Private Xvfb: `/home/ubuntu/.cache/uma-e0-xvfb/root/usr/bin/Xvfb :99 -screen 0
1280x720x24 -nolisten tcp -ac`; required X11 Python dependencies remain documented
in the external package. E0-enabled smoke uses the recorded executable wrapper to
export E0_CAPTURE after the independent tool deliberately clears source-specific
instrumentation env. Set E0_MAX_BYTES=12884901888, select a new RAM capture directory
and optional E1_TRACE, then pass the wrapper as --binary. Record actual binary hash
separately. No producer/harness source modification is needed.

## Verdict and next admission

**E1B scalar completion: GREEN** for the admitted scalar contract, prefix isolation,
CP/VO progress, store-before-IRQ, owned lifetime/error/shutdown tests and clean short
smoke. This is not complete E1 visibility correctness or production performance
acceptance. Remaining work is exactly **E1C — GDS / packet-specific visibility
completion**. Do not start shared backing until its prerequisites are established.

Implementation commits (all from the exact E1A base):

- 424c1f15 — successful prefix receipt / detach adapter.
- a3e4a96e — owned scalar descriptor, cancellable lane, typed completed proof.
- 7df40b2b — CP delivery and scalar packet integration / checked stores.
- 5a024c7e — serialized teardown and validation counters.
- a8f17421 — production CP ownership adapter and targeted evidence analyzer.
- f43cdbb6 — terminal wait error cannot be masked by prior completed proof.
- 22785b9a — checked VMM contention retries preserve CP progress.
- 0ab8e838 — existing synchronous Finish cannot mint proof on wait failure.

Changed production files: common/checked_write; core/memory; GnmDriver stop hook;
Emulator shutdown; Liverpool + scalar_completion; Scheduler + submitted_prefix;
guest_completion + cp_completion_target; Rasterizer/TextureCache completed proof
propagation; Presenter teardown and opt-in vk_prefix_test. Tests: tools/e1b_tests
and adapted E1A packet probe/tests/README. No BufferCache, MemoryTracker, VMM map
operations, shader translation, E0 producer/analyzer or external harness changes.

Protected E0/E0b/E1A/deathloop source worktrees remain clean. The external research
repository's existing tracked ROADMAP.md/benchmark-runner edits were left intact.
