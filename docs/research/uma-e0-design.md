# E0 runtime census (observation only)

Base: upstream main f73709cada28e77523e37806fee5c454711d24ba, verified with
`git ls-remote https://github.com/shadps4-emu/shadPS4.git refs/heads/main` on
2026-10-04. This work is independent of the immutable E0b checkout and of old
UFFD/coherence patches. No shared backend or guest synchronization changes.

## Questions and facts

Measure successful raw mapping sizes, backing segments/aliases, live mapping
fragmentation, mapping churn/lifetimes and GPU-touched mappings. Compare mapping
boundaries with the *runtime* BufferCache block size. Compare raw mappings with
coalesced import-region candidates; calls are not necessarily future imports.
Measure recorded buffer ranges, snapshots, shader DMA resident-set candidates,
recording sessions, real submits and completion observation. Measure packet parse,
label/store and IRQ times for E1, without delaying them. Report CPU write/read and
mapping overlap evidence with graded confidence; absence of Disabled-mode RAW
observations must never establish absence of RAW hazards.

Hooks, verified against this base:

* MemoryManager successful MapMemory/PoolCommit before VMA coalescing: raw call
  range and clipped physical pieces are facts. UnmapBytesFromEntry and ProtectBytes
  expose the actual affected subranges. VMA samples after coalescing expose actual
  region sizes; offline coalescing is a candidate, not an importability proof.
* BufferCache ObtainBuffer/ObtainBufferForImage and upload-copy construction:
  recorded intent and record-time snapshot ranges. These are conservative use
  candidates, not proof of shader execution. SynchronizeDmaBuffers reports the
  resident set; exact DMA/BDA shader access remains unknown.
* Scheduler BeginSession and SubmitExecution: capture-local context/session IDs,
  explicit session membership, NextTick signal value and queue-submit result.
  CurrentTick remains prospective. Completion observations wait on the existing
  semaphore only after successful submit, with finite vkWaitSemaphores timeouts;
  no Scheduler::Wait/Flush/Finish, scheduler lock or added device ordering.
* Liverpool EOP/EOS/ReleaseMem parsing, actual label writes/interrupt callbacks,
  WriteData and renderer CPU DmaData fast paths: facts about CP handling times.
* PageManager raw fault and existing watcher updates, plus BufferCache dirty/read
  seams: fault size is an upper bound, not decoded instruction width. Watcher
  counts/transitions are captured under existing locks; raw fault does not read
  mutable PageState. BufferCache attribution is established only by paired tracker
  evidence; ambiguous/missing pairing remains uncertain. No additional protection
  or watchers. Precise download observation precedes the existing Finish.

## Transport and safety

Opt-in environment variable SHADPS4_UMA_E0_CAPTURE selects a new output directory.
Disabled hooks take one atomic enabled check. Linux fault producers use only
lock-free atomics, fixed POD stores, clock_gettime and gettid syscall. A bounded
preallocated MPSC queue has bounded reservation attempts and explicit drop counts.
No allocation, formatting, I/O, mutex or container growth is added to a fault path.
The collector does binary I/O and metadata JSON on its own thread, with a file-size
budget. Fault producers preserve errno. Collector storage survives late faults.
Timeline observers are per scheduler and stop/join before semaphore destruction.
Output is bounded; observer backlog may coalesce observations, which remains an
explicit completion-timestamp upper bound.

## Format, reconstruction, limits

Versioned fixed-width little-endian binary records carry monotonic nanoseconds,
sequence, thread, event type/flags, context/session/tick, range and three auxiliary
fields. Metadata records build/source, config/tracking, GPU/driver, OS, game and
harness inputs, wall-clock start/end and drops. Raw events remain available.
The offline analyzer reconstructs epochs from successful maps, clips lifetimes on
unmap/replacement/protection, joins session membership to successful actual submits,
and indexes completion observations separately. Lost records, missing sessions,
unsent observations and incomplete tails invalidate exact/negative claims.
Snapshots cannot be changed by later guest writes. Current mirrored buffers can
mask future shared-memory hazards. Overlap is a strong proxy/upper bound, not proof
of actual GPU access; observed completion is later than or equal to GPU completion,
so an unobserved completion does not prove GPU work is pending. Alias-VA CPU writes
and exact DMA accesses remain unobservable; backing aliases supply upper bounds.
No hot-path JSONL. Expected overhead is clock/atomics/POD copy per event plus
collector bandwidth; measure off/on controls before making performance claims.

## Existing automation

Inventory: tools/uma_e0/inventory-20261004.json. Historical runner:
/home/ubuntu/bb-shadPS4-correctness-instrumentation/tools/run_bb_death_reload_benchmark.py.
It requires old emulator-side SHADPS4_BB_DEATH_LOOP/LOAD_DIAG callbacks, so it is not
valid unchanged against upstream. Reusable independent input/screenshot helper:
/media/ubuntu/UsbSSD447G/shadps4/work/bb-1.09-exploration/diagnostics/x11_control.py
and its x11-venv Python. .bbpad replays also require old source support. Do not
cherry-pick those implementations. Use external X11 input and bounded launches;
full captures must confirm Central Yharnam/death/reload via operator or screenshots.

Scope corresponds to a fresh BB-INS2 runtime producer experiment; it does not
promote earlier baseline-bound schemas or historical Bloodborne evidence.

## Verified hook corrections (2026-10-04)

Command sequence is separate from sessions and ticks: ObtainBuffer and upload
observations inherit an outer draw/dispatch/copy command ID. The actual Vulkan
recording hook latches the scheduler instance, session and CurrentTick then, and
session membership is joined to real NextTick submits. Runtime upload/copy
commands also record their actual recording session. Presenter submits on the
same draw scheduler; it must remain in that context's timeline. Sparse arena bind
submissions record memory_semaphore signal and the owning graphics submit tick.

Every existing Scheduler::Finish records begin/end and a caller tag. In particular,
OnFence image-download Finish can precede SignalFence, and EOS GdsStore has an
existing Finish. Fence summaries separate those cases from labels/IRQs issued
before observed completion; missing completion is not proof of pending execution.

TryWriteBacking writes backing PA directly without a protection fault or cache
notification and affects all aliases. Emit each physical write piece immediately
before memcpy, with originating guest VA and normal-context origin tag. Join
physical pieces to live GPU-touched mappings and buffer-mirror holds, reporting
possible existing mirror staleness separately from host-timeline overlap.

The binary record gains a thirteenth u64, cmd_seq (104 bytes). No unsafe-context
producer accesses mutable scheduler/session or watcher containers.

## Source inspection refinement

On this base, IsValidGpuMapping tests the 40-bit address bound, independent of
MemoryProt::GpuRead/GpuWrite. Census candidates therefore include mapped memory
below that bound and explicit renderer map/unmap requests. PoolDecommit,
NameVirtualRange and SetDirectMemoryType metadata are observed as well. Actual
post-merge VMA samples are reported separately from inferred candidate regions.
