# E1A test scaffolding

Exact base: 2437a5538bbc4fdffed8cf04bdb06ba08f91e095.
Production code is unchanged. See [design](../../docs/research/uma-e1-design.md).

```bash
python3 -m unittest discover -s tools/e1a_tests -p 'test_*.py' -v
```

Requires Python3 and a C++23 compiler (CXX, otherwise clang++-19/clang++/g++).
Builds a tiny real-header CPU-only PM4 probe in a disposable temporary directory.
No CMake/submodules, Vulkan device, proprietary data or game are required. Shims
replace only assertion/clock service dependencies, never packet or scheduler logic.

Expected at E0: 21 tests, 16 passes and **5 intentional expected failures**.
To demonstrate the deliberately red desired assertions without XFAIL masking:

```bash
python3 tools/e1a_tests/run.py --strict-current
```

At exact E0 this intentionally exits1 with five assertion failures. It is not the
normal green scaffolding command. Those failures witness actual helper parse-time
side effects. The helper alone
cannot test a future delayed caller; E1B must bind assertions to its actual adapter.
Fake scheduler/model tests are executable desired-invariant designs, not proof
that current or future production Scheduler/Liverpool satisfies them.

Source blobs and the bounded E0 summary projections are bound in provenance.json.
A header or path change requires re-deriving the expectation, not blindly carrying
this E0 characterization forward. No source fix is linked into emulator targets.
