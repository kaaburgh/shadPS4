# E3 / RADV DMA-BUF feasibility gate

Status: runner and assertions prepared; lavapipe and RADV have not been executed in this PR.

Related: [shadPS4 #9](https://github.com/kaaburgh/shadPS4/issues/9), [shadPS4 roadmap #15](https://github.com/kaaburgh/shadPS4/issues/15), [open-test roadmap #25](https://github.com/kaaburgh/shadps4-open-test/issues/25).

## Goal

Before modifying SharedBacking, test whether **real Linux /dev/udmabuf -> Vulkan DMA-BUF** imports on AMD/RADV preserve CPU/GPU coherence and remap lifetime. The existing E3 host-pointer backend cannot import its memfd backing on RADV.

Instead of copying the existing comprehensive E0b host probe into another branch, the runner extracts its pinned source at [e35713c](https://github.com/kaaburgh/shadPS4/tree/e35713c7e56e751a01c7d3a6606c3d6c1a65ac5b/tools/e0b_uma_probe) from the Git object database, builds it separately and checks a small, relevant subset. If a clone does not have the commit, the runner fetches the branch `research/uma-e0b-probe`, which points at it. The source is pinned so both machines run the identical probe.

This is host-only Vulkan, **not** a PS4 guest workload; OpenOrbis, Xvfb and a commercial game dump are not required.

## Prerequisites

Linux, Python 3.10+, Git, tar, CMake >=3.20, a C++20 compiler, Vulkan headers and loader, Linux API headers, and the target ICD. Example for Ubuntu:

~~~bash
sudo apt update
sudo apt install -y git cmake build-essential libvulkan-dev linux-libc-dev \
    mesa-vulkan-drivers vulkan-tools
# Optional when passing --validate:
sudo apt install -y vulkan-validationlayers
~~~

No root, kernel-module insertion, group change or sysctl modification is attempted by the runner. On RADV, missing /dev/udmabuf or insufficient access becomes a reported BLOCKED result rather than an apparent shared-memory success.

## Host-only unit checks (no Vulkan required)

From the PR checkout:

~~~bash
python3 -m unittest tools/test_uma_e0b_preflight.py -v
python3 -m py_compile tools/uma_e0b_preflight.py \
    tools/test_uma_e0b_preflight.py
~~~

These fixtures ensure that wrong driver selection, missing/unsupported cases, real FAIL/ERROR, validation errors and duplicate entries cannot be counted as a GO.

## Lavapipe self-test

~~~bash
python3 tools/uma_e0b_preflight.py run --mode lavapipe \
    --out "$PWD/e0b-lavapipe-evidence"
~~~

Optional validation pass (requires installed Vulkan validation layers):

~~~bash
python3 tools/uma_e0b_preflight.py run --mode lavapipe --validate \
    --out "$PWD/e0b-lavapipe-validated"
~~~

The default ICD is /usr/share/vulkan/icd.d/lvp_icd.json (override via --icd). The runner passes --allow-cpu and --selftest-memfd-as-dmabuf to the pinned probe. **The latter supplies an ordinary memfd as a fake DMA-BUF handle. It does not invoke UDMABUF_CREATE.**

A valid result is SELFTEST_PASS_NOT_REAL_DMABUF with all four of:
- T4b.planb_coherence_stress / dmabuf/bda: PASS
- T4b.planb_coherence_stress / dmabuf/ssbo: PASS
- T5b.planb_remap_identity / dmabuf/bda: PASS
- T5b.planb_remap_identity / dmabuf/ssbo: PASS

The driver must actually be a CPU Vulkan device, the source must say self-test memfd (fake), there must be no device loss, and there must be no validation errors (when enabled). A green lavapipe result is **only a self-test of the E0b test logic**, not evidence of real AMD DMA-BUF capability.

## Real AMD/RADV gate

On an AMD Linux machine:

~~~bash
ls -l /dev/udmabuf || true
cat /sys/module/udmabuf/parameters/size_limit_mb 2>/dev/null || true
vulkaninfo --summary

python3 tools/uma_e0b_preflight.py run --mode radv \
    --out "$PWD/e0b-radv-evidence"
~~~

Use --device N if multiple devices are exposed. The runner requests RADV, then **checks actual JSON device facts**: AMD vendor 0x1002, driver name containing radv, real discrete/integrated GPU, and DMA-BUF source exactly /dev/udmabuf. CPU lavapipe or AMDVLK is rejected. There is no fake-memfd option in this mode.

RADV_IMPORT_PROTOTYPE_GO requires all four actual T4b/T5b variants PASS, no device loss, and no validation errors. This gates *a prototype of the importer*, not enabling it by default: 64–256 MiB import sizing, pin/GTT budget, failed import fallback, synchronization and performance still need testing later on real hardware.

| Verdict | Meaning |
| --- | --- |
| SELFTEST_PASS_NOT_REAL_DMABUF | lavapipe fake-fd self-test passed; still run RADV |
| RADV_IMPORT_PROTOTYPE_GO | real RADV small-buffer coherence and remap tests passed; importer prototype justified |
| BLOCKED | unavailable /dev/udmabuf or a required test is UNSUPPORTED, SKIP or missing |
| WRONG_DEVICE | selected GPU/driver/source does not match mode |
| FAIL | probe data mismatch, validation error, device loss, duplicate evidence or nonzero probe failure |
| INFRA_FAIL | source fetch/build/execution/JSON collection could not complete |

## Evidence and exit status

Each run requires a *new* output directory and writes:

- manifest.json: selected mode, pinned source SHA, host summary, requested device and validation
- configure.log, build.log, probe.log
- probe.json: native E0b JSON, including per-case PASS/FAIL/UNSUPPORTED and device facts
- decision.json: independent machine-readable classification

Exit code: 0 means self-test success or RADV prototype GO; 1 means actual FAIL; 2 means unsupported/incorrect environment/infra. Always read the exact verdict, **not just exit code 0**.

An existing report can be reclassified without a GPU:

~~~bash
python3 tools/uma_e0b_preflight.py classify --mode radv \
    e0b-radv-evidence/probe.json
~~~

Attach probe.json, decision.json and probe.log to the PR or shadPS4 #9. They include host/device facts; inspect them before sharing.

## Limitations and next steps

- No changes to the actual emulator or its import path in this PR; no performance measurements.
- This gate exercises ordinary Vulkan VkBuffer imports, not sparse residency, TextureCache, BDA remap in shadPS4, or full GPU memory pressure.
- The fake lavapipe handle is **not** equivalent to kernel udmabuf on RADV.
- After actual RADV GO: implement smallest Linux importer behind current E3 opt-in, keep mirrored fallback, use real backend/path instrumentation; then use existing shadps4-open-test guest work, not a new test suite.
