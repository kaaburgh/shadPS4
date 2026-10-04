# E0 implementation and validation handoff — 2026-10-04

Upstream main was verified remotely before work:
`f73709cada28e77523e37806fee5c454711d24ba`.
New source: `/media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-e0`, branch
`research/uma-e0-census`, remote `fork` = `https://github.com/kaaburgh/shadPS4.git`.
Design-first commits: `d8e993db` and correction `76a61987`.
The first producer smoke was built from the second design commit plus dirty E0
implementation; this is explicitly exploratory-unverified, not source promotion.
The final committed build and its short checks are recorded in the final chat.

## Inventory and reuse

[Inventory JSON](../../tools/uma_e0/inventory-20261004.json) reports absolute roots,
full HEADs, branches, clean/dirty state and upstream `behind/ahead` counts.
All eight prior shadPS4 trees were clean at inventory. E0b remains detached at
`e35713c7e56e751a01c7d3a6606c3d6c1a65ac5b`; no worktree files were changed there.
The independent investigation repository has pre-existing tracked and untracked
changes and was read only. Existing branches/builds/runs/save/cache state were
preserved. Fetch updated shared remote refs, without checking out old branches.

Relevant old instrumentation remains in these branches:
`bb/bb-thread2-semantic-writer-20260924`,
`diag/memory-coherency-traffic-telemetry-20260924`,
`diag/memory-telemetry-bbloaddiag-20260925`,
`diag/memory-telemetry-window-attribution-20260925`,
`diag/bb-memory-telemetry-window-attribution-20260925`,
`diag/bb-post5100-lifecycle-20260925`,
`diag/bb-post5100-memory-telemetry-20260925`. These contain prior performance,
coherency, logical-traffic, lifecycle and vertex-explosion diagnostics. They were
not imported into E0. Existing launch scripts include
`work/bb-1.09-exploration/diagnostics/run-pass17-streamprov.sh`,
`run-pass19-chain.sh`, `run-pass20-prov.sh`, and
`work/bb-1.09-exploration/run-manual-long-recording.sh` under the shadPS4 area.
Their old source-dependent instrumentation/controls are not valid on upstream.

The historical death/reload runner and exactly reused external helpers are
listed in [the operator handoff](../../tools/uma_e0/README.md). Its config override
class is reused verbatim; X11 input and screenshot helper is reused externally.
The old full death-loop callbacks and .bbpad source support were not ported.

## Build and tests actually run

Fresh **E0** build directory (existing unrelated builds preserved):
`/media/ubuntu/UsbSSD447G/shadps4/shadPS4-build-uma-e0`.
Submodules initialized at this base, then configured with Ninja, Clang 19,
RelWithDebInfo and bundled dependencies. Incremental builds used 12 jobs.

```bash
cmake -S /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-e0 -B /media/ubuntu/UsbSSD447G/shadps4/shadPS4-build-uma-e0 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_COMPILER=clang++-19 -DCMAKE_C_COMPILER=clang-19 -DENABLE_TESTS=OFF
cmake --build /media/ubuntu/UsbSSD447G/shadps4/shadPS4-build-uma-e0 --parallel 12
python3 -m unittest discover -s /media/ubuntu/UsbSSD447G/shadps4/shadPS4-uma-e0/tools/uma_e0 -p 'test_*.py' -v
```

E0 analyzer: **7 tests passed**. Cases cover actual consumer ticks differing from
ObtainBuffer ticks, runtime copy/fill consumers, scheduler separation/failed
submits, snapshot WAR exclusion, physical alias mirror holds, existing Finish vs
before-submit labels, drops and truncation.

Upstream existing tests: configure the same build with ENABLE_TESTS=ON (upstream
uses mutually exclusive emulator/test targets), build and run ctest. **379 passed,
44 GCN failed out of 423**, including one SIGTRAP. No tests or shader translation
were changed. Ninja dependency audit found **zero modified source inputs** in all
four upstream test targets: GCN 1073 tracked inputs, settings 206, NGS2 214, HTTP
223. These failures exercise unchanged upstream code on this NVIDIA host and are
retained as a validation limitation. They were not fixed in an instrumentation PR.
The build was subsequently restored to ENABLE_TESTS=OFF and rebuilt.
Private logs: `/tmp/uma-e0-tests-build.log`, `/tmp/uma-e0-ctest.log`.

Transport synthetic controls:

* Four producers, 40,000 synthetic records: **40,001** including End, **0 drops**.
* Four producers plus **1,000 real SIGUSR2 handler emissions**: **41,001** records,
  **0 drops**, parseable, clean shutdown.
* 4,096-byte budget: **39 written**, **40,962 dropped**, exact 4,056-byte file;
  analyzer marks transport incomplete, preserving the visible drop count.
* Disabled Emit hook: one-million-call synthetic microbenchmark measured
  **0.924 ns/call** in one run. This is hook-only evidence, not game FPS overhead.
  Disabled instrumentation creates no collector/observer threads, output directory
  or producer storage growth. Existing renderer paths retain their decisions.

No signal/fault hook emits JSON, logs, allocates or acquires a new ordinary lock.
Writer JSON/file I/O occurs on its collector or normal initialization/shutdown.
Observer source uses only direct existing-semaphore waits with finite timeout and
atomic submitted-tick queue; no Scheduler::Wait/Flush/Finish or submit lock.

## Short Bloodborne runtime smoke (no death/reload campaign)

Disposable target with receipt:
`/media/ubuntu/UsbSSD447G/shadps4/games/CUSA03173-1.09-DISPOSABLE`.
Working directory:
`/media/ubuntu/UsbSSD447G/shadps4/work/bb-1.09-exploration`.
Actual producer header: **CUSA03173, 01.09; signals; NVIDIA GeForce RTX 5070 Ti;
Vulkan driver 595.91.7.0 (operator driver 595.91.07); runtime block size 65,536**.
Both modes used the same selected configuration except temporary readbacks mode.
All launches used bounded upstream IPC STOP and exited 0 without forced kill.
No device loss or observer wait failures were found. Raw logs contain normal
upstream warnings; no claim of clean Vulkan validation-layer coverage is made.

Historical :1/GDM authorization was unavailable (connection refused / permissions).
System Xvfb installation failed because `sudo -n` required a password. Resolved
without privileged changes:
`apt-get download xvfb` in `/home/ubuntu/.cache/uma-e0-xvfb`,
`dpkg-deb -x xvfb*.deb root`, then
`root/usr/bin/Xvfb :99 -screen 0 1280x720x24 -nolisten tcp -ac`.
This private display preserved the RTX Vulkan device, verified in capture header.
Use this route for future bounded unattended smokes; use the real operator display
for interactive full captures. Do not infer inability to run the project from :1.

| Capture | Budget/run | Result |
| --- | --- | --- |
| instrumentation off | 12 seconds, Disabled | exit 0, renderer compiled/recorded game work, no census directory |
| Disabled on | 20 seconds | 157,464 events; 1,771 submits/completions; 0 drops |
| Precise on | 20 seconds | 161,548 events; 2,277 submits, 2,276 completion observations; 0 drops; one tick unobserved at shutdown |

Both on captures: 84 raw mapped epochs, 5 GPU-touched candidate epochs (peak 5),
peak 4 touched merged-import candidates; all Buffer records joined to successful
submits after the analyzer included actual runtime copy/fill consumers. This is
startup/title behavior only, **not** the Central Yharnam census. The analyzer's
initial copy/fill omission was corrected, with a regression; no unknowns were
silently treated as submitted. Disabled recorded 10,703 label/IRQ observations
before their associated host submit; Precise recorded 10,661. This establishes
that those **associated ticks** were not completed at signal time. It does not
independently prove which guest synchronization contract each label requires.
Precise had 502 existing buffer-download Finish calls and 502 backing-write
mirror-hold overlaps from those downloads, reported as possible staleness rather
than stale-byte proof. No new waits were added. Synchronous image Finish and EOS
GdsStore Finish are implemented hooks but were not exercised in this short run;
fence-class splitting is independently covered synthetically.

Artifacts (private; not committed):
`/media/ubuntu/UsbSSD447G/shadps4/runs/uma-e0-off-smoke`,
`uma-e0-disabled-smoke`, `uma-e0-precise-smoke`, `uma-e0-transport-smoke`,
`uma-e0-signal-smoke`, `uma-e0-budget-smoke` under the same runs root.
The first transport header had upstream's empty SCM SHA for linked worktrees;
E0 now obtains a full configure-time source SHA directly with git rev-parse.
Final clean-source smoke artifacts are distinguished in the final report.

## Blind spots and recommendation

Exact GPU shader accesses are not established by Buffer intent. DMA/BDA shader
uses and resident-set candidates are captured, but exact ranges remain unknown;
that path was not exercised at startup. Alias-VA guest CPU writes have only upper
bounds; direct backing writes are enumerated by PA across current aliases. RAW
absence in Disabled is unobservable, not a negative finding. Completion-observed
ns is an upper bound; late/unseen observation does not prove GPU work is pending.
Post-remap CPU overlap with retired epochs can be missed even though removal
records retain mapping-hazard evidence. Stream snapshots are excluded from later
CPU-write WAR classification. Mixed image preparation/outer draw associations
remain conservative if internal consumers cross recording sessions. Events lost
to budget/contention invalidate exact and negative claims. No instrumented/off
FPS campaign, strict promotion manifest, long campaign or E1 change was run.

**A. E0 data is sufficient to proceed to E1 timeline design.** The recorder and
short real-target data establish parse-before-submit signals and retain existing
Finish exceptions without changing semantics. Full paired Central Yharnam captures
remain the operator's next collection action and are needed for representative
E2 mapping/churn sizing; startup counts must not substitute for those results.
Do not implement E1 or shared buffers as part of this branch.
