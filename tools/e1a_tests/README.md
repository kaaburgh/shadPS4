# E1A scaffolding, bound to E1B production scalar adapters

Historical E1A base: 2437a5538bbc4fdffed8cf04bdb06ba08f91e095;
E1A design commit: 8e2a780767ce26840ba73d9d7bb0835f85e91436.
The original scaffolding had 16 passes and five expected failures. On this E1B
branch, four scalar desired assertions execute the actual owned-packet/lane
adapter. They now pass because the lane retains publication until a successfully
submitted ticket completes, not because assertions were weakened. The retained
ReleaseMem-GDS helper assertion remains an E1C expected failure.

```bash
python3 -m unittest discover -s tools/e1a_tests -p 'test_*.py' -v
```

Expected here: **21 tests, 20 pass, one intentional expected failure**.
`python3 tools/e1a_tests/run.py --strict-current` intentionally exits1 for that
remaining Release-GDS gap. Requires Python3, a C++23 compiler and pthreads. Shims
replace assertion/clock service dependencies only. Model-only tests remain
executable design examples; additional real production and Vulkan adapter tests
are in [E1B tests](../e1b_tests/README.md).

provenance.json preserves historical E1A source/evidence identities; it is not a
build manifest for E1B. See [E1B implementation/validation](../../docs/research/uma-e1b-scalar-completion.md).
