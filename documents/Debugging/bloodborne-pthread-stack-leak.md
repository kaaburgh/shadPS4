<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Bloodborne host pthread stack leak — investigation record and fix specification

> **Status: root cause proven. Production fix NOT implemented.**
> Last verified against: `a5d307e` (`shader_recompiler: Implement support for base
> vertex/instance in indirect draws (#5028)`).

---

## 0. How to use this document

This is a handover record for anyone — human or agent — picking up the Bloodborne
memory-growth investigation. It exists because the work drifted once already: a
prerequisite task was mistaken for the fix, shipped as a pull request, and the
original leak stayed open.

Read sections 1–5 before writing any code. Section 8 lists approaches that were
already tried and rejected, with the reasons; re-deriving them wastes a cycle.

Every file/line reference below was verified against the commit named above. If
you are working from a newer base, re-check the anchors before trusting them.

---

## 1. Two tracks — keep them separate

| Track | What it is | Status |
| --- | --- | --- |
| **A. Bloodborne pthread stack leak** | Host pthread stacks are never reclaimed. 13 stacks leaked per in-game reload. | Root cause **proven**. Fix **not started**. Blocked on A1/A2 below. |
| **B. POSIX `SIGSLEEP` pause protocol** | `PauseGuestThreads`/`ResumeGuestThreads` use a standard signal as a toggle, which can coalesce; new guest threads get stranded. | Bug **confirmed** (and worse than originally described). Branch `fix/posix-pause-state-protocol` is **not shippable**. |

**B is not a prerequisite for A.** This was the drift. The two tracks are
orthogonal; see section 4. B becomes near-trivial *after* A1 lands — the
dependency arrow points from A to B, not the other way round.

---

## 2. Symptom and measurement

Each Bloodborne reload creates a new generation of 13 `CSChr*` / `CSCloth*`
guest threads. The old thread IDs disappear, but their stack mappings do not.

Measured delta per reload:

```
0x680d000 = 109 105 152 bytes
13 × (8 MiB stack + 4 KiB guard) = 109 105 152 bytes   ← exact match
```

The geometry identifies the mappings unambiguously as **host** pthread stacks:

| | Stack size | Guard | Page granularity |
| --- | --- | --- | --- |
| Guest stack (`pthread.h:201-204`) | `ThrStackDefault` = 1 MiB | `ThrGuardDefault` = 16 KiB | `ThrPageSize` = 16 KiB |
| Host stack (glibc default) | `RLIMIT_STACK`, typically 8 MiB | one host page = 4 KiB | 4 KiB |

8 MiB + 4 KiB is the glibc signature for `pthread_create(attr = nullptr)`.
Guest stacks would show 1 MiB + 16 KiB.

Corroborating observations from the original investigation:

- Selectively reaping one thread produced `+12` instead of `+13`.
- Reaping all 13 held the stack-VMA count at `86 → 86 → 86` across reloads.

### Cheap read-only re-check

```bash
PID=$(pidof shadPS4)
# anonymous rw-p mappings of >= 8 MiB (host stack candidates)
awk '$2 ~ /^rw-p/ && $6 == "" {split($1,a,"-");
     if ((strtonum("0x"a[2])-strtonum("0x"a[1]))/1048576 >= 8) n++} END{print "stacks:", n}' \
    /proc/$PID/maps
ls /proc/$PID/task | wc -l     # live tasks
```

Confirmation signature: the stack count grows by ~13 per reload while the task
count returns to baseline. If the *task* count also grows, the threads are still
alive and this is a different bug (see section 10).

---

## 3. Root cause (proven)

`src/core/thread.cpp:29-38`:

```cpp
int NativeThread::Create(ThreadFunc func, void* arg) {
    pthread_t* pthr = reinterpret_cast<pthread_t*>(&native_handle);
    return pthread_create(pthr, nullptr, func, arg);   // attr == nullptr -> JOINABLE
}
```

The host thread is created **joinable**. Across the whole codebase there is:

- no `pthread_join` on that handle,
- no `pthread_detach`,
- no `pthread_attr_setdetachstate(PTHREAD_CREATE_DETACHED)`,
- an empty `NativeThread::~NativeThread()` (`thread.cpp:27`).

(`grep` finds only the *guest* `posix_pthread_join` / `posix_pthread_detach`,
which are emulation entry points for the game, not host calls.)

POSIX is explicit: for a non-detached thread, storage — including the stack — is
not reclaimed until `pthread_join`. In glibc the release path is
`__free_tcb` → `__deallocate_stack`, reached either from `pthread_join` or from
`start_thread` when `IS_DETACHED(pd)`. Neither happens here, so the stack is not
even returned to glibc's `stack_cache` for reuse. Growth is therefore unbounded
rather than plateauing at `stack_cache_maxsize` (40 MiB).

`ThreadState::Free` (`thread_state.cpp:120-144`) recycles the `Pthread` object
into `free_threads`; on reuse `posix_pthread_create_name_np:358` assigns a fresh
`std::make_unique<Core::NativeThread>`, destroying the old one. The `pthread_t`
is simply dropped.

### Context worth noting

Guest-side per-thread resources *are* handled carefully — guest stacks are
recycled into `dstackq`/`mstackq` (`stack.cpp:122`), collected by
`ThreadState::Collect` (`thread_state.cpp:33-51`), and the 64 KiB sigaltstack is
freed in `NativeThread::Exit` (`thread.cpp:53-62`). The host pthread stack is the
one per-thread resource nobody owns.

### Windows

`CreateThread`'s `HANDLE` is never `CloseHandle`d either (`Exit()` nulls
`native_handle` and calls `ExitThread(0)`). Windows frees the stack at thread
exit, so the symptom there is a leaked kernel thread object, not 8 MiB per
thread. Lower priority, same root cause class, same fix owner.

---

## 4. Why `fix/posix-pause-state-protocol` is not this fix

That branch touches nine files:

```
src/core/debug_state.cpp   src/core/pause_protocol.h   src/core/signals.cpp
src/core/debug_state.h     src/core/signals.h          tests/ (4 files)
```

It does not touch `src/core/thread.cpp`, `pthread.cpp`, `thread_state.cpp` or
`stack.cpp` — none of the code where the leak lives. The leak also happens on the
normal, successful thread-exit path, with no pause involved: `PauseGuestThreads`
is the only source of `SIGSLEEP`, so in a session where nobody paused, the pause
machinery never ran at all.

The branch was scoped as "prerequisite #1" for the detached-native design, on the
theory that the `SIGSLEEP` coalescing race blocked it. **That was the wrong
prerequisite.** See section 5.

The branch does contain real, independently valid work — a correct diagnosis of
the pause bug, and an integration test harness that compiles the real
`debug_state.cpp` / `signals.cpp`. Treat it as a source of requirements and test
scaffolding, not as code to merge. It currently carries a Windows self-suspend
deadlock, a macOS build break, and a regression that disables pause entirely
after the first `scePthreadExit`.

---

## 5. Real blockers, in dependency order

Today a stale `pthread_t` is harmless: because host threads are joinable and
never joined, their TCB is never freed, so `pthread_t` (a pointer to the TCB at
the top of the stack, on glibc) stays a valid pointer forever. `pthread_kill` on
a dead thread returns `ESRCH`. This is *why* the current code appears to work
while holding stale IDs in several places.

The moment threads become detached, glibc frees the TCB at exit and the stack is
recycled. Every stored `pthread_t` becomes a dangling pointer into reusable
memory, and `pthread_kill` on it becomes a use-after-free that may deliver a
signal to an unrelated thread.

So the actual prerequisite is:

> **No stored `pthread_t` may be used after its thread has exited.**

That is blocker **A1**. Blocker **A2** is the alternate-signal-stack teardown on
the exit path (section 6, step 2). Neither is addressed by the pause branch.

```
planned order                    order implied by the code
─────────────────                ──────────────────────────
1. SIGSLEEP prerequisite         1. A1  safe host-thread handle
2. altstack prerequisite         2. A2  altstack-safe exit
3. detached-native lifetime      3.     detached-native   (trivial once A1 holds)
4. ...                           4.     pause fix         (~30 lines, falls out)
```

A safe host-thread handle is exactly what `PauseGuestThreads`,
`InterruptPthreadForCancellation` and `WakeForSignal` all lack. Once it exists,
the pause fix no longer needs epochs, acknowledgements, rollback records or
per-thread pipes.

### Good news: guest join semantics are already emulated

`JoinThread` (`pthread.cpp:113-205`) waits on `pthread->join_wait_cv` and
`pthread->tid == TidTerminated`. It never calls host `pthread_join`. So
"guest joinability != host joinability" is a description of what already exists,
not an architecture to build. Detaching the host thread changes nothing in guest
semantics.

---

## 6. Fix specification

### Step 0 — remove the source of stale thread IDs (one line, independently useful)

`ExitThread` (`pthread.cpp:45`) is the single funnel for every exit path: normal
return from `RunThread`, guest `scePthreadExit`, and cancellation
(`PthreadTestCancel` → `posix_pthread_exit`). Today `RemoveCurrentThreadFromGuestList`
sits in `RunThread:276`, *after* `posix_pthread_exit`, so it is skipped for two of
those three paths — `ExitThread` ends in `native_thr->Exit()` + `UNREACHABLE()`.

Move it:

```cpp
static void ExitThread() {
    Pthread* curthread = g_curthread;
    DebugState.RemoveCurrentThreadFromGuestList();   // <- here, before anything else
    if (curthread->specific != nullptr) { _thread_cleanupspecific(); }
    ...
```

and delete the call from `RunThread`. `pthread.cpp` already includes
`debug_state.h` (it calls `AddCurrentThreadToGuestList` at `:260`).

This alone stops `DebugState::guest_threads` from accumulating 13 dead entries per
reload. Verify with `TestGuestThreadCount()` returning to baseline after a reload.

### Step 1 — A1: lifetime-safe host thread handle

Goal: make "use the thread ID" and "the thread finishes exiting" mutually
exclusive, without a blocking primitive on the hot path.

**Preferred (Linux and friends): kernel TID + `tgkill`.** Safe by construction
rather than by lock discipline, because it passes an integer instead of a pointer.

```cpp
class NativeThread {
    std::atomic<u64> kernel_tid{0};   // gettid(), NOT pthread_self()
};

// Initialize(), on the thread itself:
kernel_tid.store(::gettid(), std::memory_order_release);

// Exit(), on the thread itself, as the FIRST action:
kernel_tid.store(0, std::memory_order_release);

// any sender:
bool NativeThread::Signal(int sig) noexcept {
    const u64 t = kernel_tid.load(std::memory_order_acquire);
    if (t == 0) return false;                  // exited: no-op, not an error
    return ::syscall(SYS_tgkill, ::getpid(), (pid_t)t, sig) == 0;
}
```

A missed race cannot dereference freed memory. Worst case is `ESRCH`, or hitting
a recycled TID — which requires the kernel's TID counter to wrap (`pid_max`,
typically 4 194 304). If even that is unacceptable, pair the TID with a
generation counter and re-check after sending.

The residual "signal reached a recycled TID" case is already neutralised by the
receivers: `PauseSignalHandler` checks `CurrentParticipant != nullptr` and
`PthreadCancelInterrupt` checks `g_curthread != nullptr`, so an unrelated thread
no-ops. **Write that down as an invariant in the code** rather than leaving it to
chance.

Notes:
- `gettid()` needs glibc 2.30+; otherwise `syscall(SYS_gettid)`.
- `NativeThread::tid` already exists and is *almost* this flag: it is set to
  `(u64)pthread_self()` in `Initialize()` and zeroed in `Exit()`. Nothing checks
  it at the kill sites. Formalising it is the change.

**Portable fallback (macOS, where there is no `tgkill`):** a `std::shared_mutex`
gate inside `NativeThread`. Senders take a shared lock and check an `alive` flag;
the exiting thread takes the unique lock before `pthread_exit`. This requires the
`NativeThread` object to outlive the thread, i.e. `shared_ptr` ownership instead
of the current `std::unique_ptr` in `Pthread` (`pthread.h:288`) — otherwise
`ThreadState::Free` can destroy it under a sender holding the shared lock.

Route **all** consumers through the handle: see Appendix A.

### Step 2 — A2: altstack-safe exit

`NativeThread::Exit()` (`thread.cpp:52-63`) currently does:

```cpp
constexpr stack_t sig_stack = { .ss_flags = SS_DISABLE };
sigaltstack(&sig_stack, nullptr);      // return value ignored
if (sig_stack_ptr) { free(sig_stack_ptr); sig_stack_ptr = nullptr; }
pthread_exit(nullptr);
```

POSIX returns `EPERM` from `sigaltstack` when the calling thread is executing on
the stack being modified. The return value is ignored, so `free()` runs anyway
and releases the stack the thread is standing on; `pthread_exit` then unwinds
over freed memory.

Reachable path (pre-existing, not introduced by the pause branch):

```
thread parked in the pause handler        (SA_ONSTACK -> running on the altstack)
        v
cancellation signal arrives (SIGRTMAX)
   its handler is deliberately NOT SA_ONSTACK (pthread.cpp:673-676, see comment),
   but we are already on the altstack, so the kernel does not switch -- it runs there
        v
PthreadCancelInterrupt -> cancel_async -> PthreadTestCancel
        v
posix_pthread_exit -> ExitThread -> NativeThread::Exit()
        v
sigaltstack(SS_DISABLE) -> EPERM (ignored)
free(sig_stack_ptr)     -> frees the stack under our feet
pthread_exit            -> use-after-free
```

Fix:

```cpp
stack_t current{};
const bool on_altstack = sigaltstack(nullptr, &current) == 0 &&
                         (current.ss_flags & SS_ONSTACK) != 0;
if (!on_altstack) {
    constexpr stack_t disable{ .ss_flags = SS_DISABLE };
    (void)sigaltstack(&disable, nullptr);
    RecycleSignalStack(std::exchange(sig_stack_ptr, nullptr));
} else {
    // The kernel stops using this stack right after pthread_exit; hand it to a
    // deferred reclaim list instead of freeing it while standing on it.
    DeferSignalStackReclaim(std::exchange(sig_stack_ptr, nullptr));
}
```

A size-keyed cache (mirroring the existing `dstackq`/`mstackq` idiom) is
preferable to `posix_memalign`/`free` per thread and removes a 64 KiB allocation
from every thread creation.

### Step 3 — detach (the leak fix proper)

```cpp
int NativeThread::Create(ThreadFunc func, void* arg) {
#ifndef _WIN64
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) return errno;
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t* pthr = reinterpret_cast<pthread_t*>(&native_handle);
    const int ret = pthread_create(pthr, &attr, func, arg);
    pthread_attr_destroy(&attr);
    return ret;
#else
    ...
#endif
}
```

Do **not** land this before steps 1 and 2. On its own it converts benign `ESRCH`
returns into use-after-free at three call sites.

Windows counterpart: close the `CreateThread` handle once every sender has
released its reference — same handle abstraction as step 1.

### Step 4 — check for a second, independent leak

`Pthread::ShouldCollect()` (`pthread.h:358`) requires the `Detached` flag:

```cpp
return refcount == 0 && state == PthreadState::Dead && True(flags & ThreadFlags::Detached);
```

| Guest thread ends as | Guest stack (1 MiB + 16 KiB) | `Pthread` object |
| --- | --- | --- |
| detached | freed into `dstackq` | returned to `free_threads` |
| joinable + joined | freed | returned |
| **joinable, never joined** | **leaks** | **leaks** |

The measured `0x680d000` is fully explained by host stacks, so the Bloodborne
threads are either detached or joined. Still, measure `dstackq.size()` and
`total_threads` across reloads once — better to find a second leak now than after
the first one is fixed and stops masking it.

---

## 7. Acceptance criteria

```
Bloodborne, 3 -> 10 reloads, cache-zero
  stack VMA count             86 -> 86 -> 86
  VA delta                    no +0x680d000 per reload
  DebugState::guest_threads   returns to baseline after each reload
  dstackq / total_threads     stable (step 4)
  RSS                         plateaus instead of growing monotonically

regressions
  guest join / detach / cancel   existing tests, plus a new one for
                                 cancellation while a thread is paused
  pause / resume                 still works after >= 1 scePthreadExit
                                 (this is what regresses on the pause branch)

platforms
  Linux x86-64 and arm64, macOS, Windows all build and start
```

---

## 8. Rejected approaches — do not re-derive these

**Native reap inside guest `JoinThread`.** Rejected across several review rounds.
Final blocker: the native reap happens before control actually returns to guest
code, and asynchronous cancellation can land in the window between the reap and
the guest return. Candidates up to `c9f173ed` were built on this shape; do not
restart from them.

**Scoping the `SIGSLEEP` coalescing race as prerequisite #1.** This is what
branch `fix/posix-pause-state-protocol` implements. It is a real bug, but it is
not what blocks the detached model — see section 5. Do not treat that branch as
a completed step of track A.

**Fixing the pause protocol with an acknowledgement protocol.** No consumer of
the pause needs a synchronous "all threads are stopped" guarantee; the old code
never provided one. Verified consumers: `gnmdriver.cpp:302, 2232` (the caller
parks itself), `layer.cpp` (reads dumps under `frame_dump_list_mutex`),
`videoout/driver.cpp:361` (switches to `DrawLastFrame`), `frame_graph.cpp`
(freezes statistics). The acknowledgement machinery is what produces the pause
branch's regressions.

---

## 9. Appendix A — every stale `pthread_t` consumer (POSIX)

All of these must go through the step-1 handle before step 3 lands.

| Site | Anchor | Call |
| --- | --- | --- |
| `InterruptPthreadForCancellation` | `pthread.cpp:695-697` | `pthread_kill(tid, HostPthreadCancelSignal())` |
| `Pthread::WakeForSignal` | `pthread.cpp:964` | `pthread_kill(tid, SIGUSR1)` |
| `NotifyPauseTarget` | `debug_state.cpp:62` | `pthread_kill(id, SIGSLEEP)` |
| `Pthread::SetAffinity` | `pthread.cpp:1102` | reads `GetHandle()`; the actual use is currently commented out |

Not a risk: `Common::SetThreadName` is a no-op TODO on POSIX
(`common/thread.cpp:215-217`). `Common::SetCurrentThreadName` only ever targets
`pthread_self()`.

The feeder of stale IDs into the third row is `DebugState::guest_threads`, fixed
by step 0.

---

## 10. Appendix B — open questions

1. Are the Bloodborne `CSChr*` / `CSCloth*` threads created detached, or created
   joinable and joined? Step 4 depends on the answer; measure rather than assume.
2. Does the task count stay flat across reloads? If it grows, some threads are
   still alive and a second mechanism is in play — that would be the one case
   where the pause-strand bug could contribute.
3. Does any platform in CI set a non-default `RLIMIT_STACK`? The 8 MiB figure is
   the typical default, not a guarantee; the fix does not depend on it, but the
   acceptance numbers do.
