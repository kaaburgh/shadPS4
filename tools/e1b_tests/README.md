# E1B production-adapter regressions

```bash
python3 tools/e1b_tests/run.py
python3 -m unittest discover -s tools/e1b_tests -p 'test_*.py' -v
python3 -m unittest discover -s tools/e1a_tests -p 'test_*.py' -v
```

C++23 and pthreads required (CXX defaults to clang++-19). The CPU suites compile
real SubmittedPrefix, GuestCompletionLane, CpCompletionTarget, packet descriptor
and VO-progress code used by Scheduler/Liverpool. Fake queue/timeline/CP mailbox
endpoints supply controlled success, error, cancellation and ordering conditions;
they do not replace the production detach, admission, gate, publication or token
logic. Assertion/clock shims are the retained E1A service shims. Assertions are on.

Coverage: all scalar packet families/selectors, store-before-IRQ, owned payload,
wrong scheduler identity, no ticket on failed submit, A/fence/B detach, idle CP,
per-queue sequencing, synchronous proof promotion, pending cancellation, ready
message invalidation, VO mutex release/progress, wait/device-loss failure including
failure after a completed proof. Evidence tests have early-store/drop negative
controls and guards for unchanged WriteData/DMA/wait/semaphore parser paths.

Real Vulkan test (isolated, exits before guest execution): run the normal binary
with SHADPS4_E1_PREFIX_SELFTEST=1 and a private copied profile. Require the explicit
`E1B real Scheduler A/fence/B + upload + Finish proof: PASS` marker and exit0;
use an external 60-second process bound. It records upload+A, submits T, records B,
waits directly for T and checks coherent host-visible test memory: upload+A visible,
B still zero. Then it submits B and checks B. An existing Finish proof enters the
lane without another wait. This binds prefix isolation to the real Scheduler API,
real queue and real timeline, beyond the fake-queue CPU endpoint.

SHADPS4_E1_TRACE=/absolute/new/file.jsonl optionally records CP publication timing
and shutdown counters. It performs ordinary file I/O only on CP/stop context,
never fault/waiter context. Omit it for normal use. Raw E0 producer/schema/analyzer
are unchanged. Targeted join:

```bash
python3 tools/e1b_tests/analyze_trace.py /path/e1.jsonl \
  --capture /path/e0-capture --output /path/e1-summary.json
```

The join uses E0 packet context + E1 accepted ticket value, never publication-time
CurrentTick. Independent E0 completion timestamps are upper bounds; they can lag
E1's successful wait and CP publication. Full E0 VMM/hazard analysis is separate.
