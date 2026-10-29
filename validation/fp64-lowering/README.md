<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Validation of kaaburgh/shadPS4#50 (fp64 lowering)

This branch is not meant to be merged. It keeps the harness and the results behind the
validation of kaaburgh/shadPS4#50, "fix the exponent bias when lowering fp64 to fp32",
at its head `48b973a`.

## Revisions

- **shadPS4:** `48b973a` (kaaburgh/shadPS4#50).
  - `3df1f80` (kaaburgh/shadPS4#52, `S_BREV_B64`) is cherry-picked without committing, because
    `f64_pack` needs it.
  - Release build, clang-19.
- **shadps4-open-test:** `905b8ce` (kaaburgh/shadps4-open-test#53, `tests/f64-pack`), with
  opengnm-psbc `365a223`.
- **Device:** lavapipe, llvmpipe 25.2.8, Vulkan 1.4.318, under Xvfb, readbacks Precise.
  - lavapipe supports `shaderFloat64`, so the lowering only runs with `force-fp64-off.patch`.
  - **Nothing here ran on a device whose `shaderFloat64` is actually false.**

## Files

| file | applies to | what it is |
|---|---|---|
| `force-fp64-off.patch` | shadPS4 | sets `support_float64 = false`, so every shader goes through `LowerFp64ToFp32` |
| `lowered-model-matcher.patch` | open-test `905b8ce` | checks the `f64_v`/`f64_s` results against an exact model of the lowering, with checked/skipped counts |
| `tolerance-matcher.patch` | open-test `905b8ce` | the earlier relative-tolerance check (1e-5) behind the counts in the first version of the PR |
| `bit-model.cpp` | standalone | bit-exact model of both conversions as the IR computes them |
| `bit-model.out` | | its output, trimmed to the edge cases, the summary and a classification of every mismatch |
| `lowered-model-coverage.txt` | | per-operation counts from `lowered-model-matcher.patch` on `48b973a` + `3df1f80` with fp64 off |

## Reproducing

```sh
# shadPS4: kaaburgh/shadPS4#50 head, S_BREV_B64, and fp64 forced off
git checkout 48b973a
git cherry-pick -n 3df1f80
git apply validation/fp64-lowering/force-fp64-off.patch   # from this branch
cmake --build build

# open-test: the boundary cases, and optionally a scratch matcher
git checkout 905b8ce
git apply lowered-model-matcher.patch                     # optional
bash scripts/build-test.sh shader_alu
SHADPS4=/path/to/shadps4 bash scripts/run-test-lavapipe.sh shader_alu --timeout 600 \
    [--set GPU.dump_shaders=true]

# bit model
g++ -O2 -std=c++20 -o bit-model bit-model.cpp && ./bit-model
```

## Results and what each one shows

### 1. Boundary cases: `f64_pack`, `f64_pack_const` (execution, exact bits)

The values are those of kaaburgh/shadps4-open-test#53. They cover exponents 896, 897, 1150 and 1151 with
zero and nonzero mantissas and both signs, plus 1.5, 2^-767, 2^-130, ±0, ±inf and a NaN, read
from a buffer and as shader constants.

| shadPS4 | mode | wrong values, `f64_pack` / `f64_pack_const` |
|---|---|---|
| `473fa84` + `3df1f80`, fp64 off | lowered | 25 / 22 of 29 |
| bias fix only + `3df1f80`, fp64 off | lowered | 5 / 5 (±2^-767, ±just below `FLT_MIN`, 2^-130) |
| `48b973a` + `3df1f80`, fp64 off | lowered | 0 / 0 |
| `48b973a` + `3df1f80` | fp64 | 0 / 0 |

### 2. Exact model of the lowering on `f64_v`/`f64_s` (execution, exact bits)

`lowered-model-matcher.patch` narrows each operand with the PR's f64-to-f32 rules and performs
the operation in f32. It then widens the result and requires the GPU result to match that bit
for bit. `abs`/`neg` are integer operations in ACO's code, so they are exact in f64. `fma`
may be fused or not. `min`/`max` of +0 and -0 may return either.

On `48b973a` + `3df1f80` with fp64 off, 4096 threads per variant:

- **Checked, 0 failures:**
  - `add`, `sub`, `mul`, `fma`, `floor`, `ceil`, `trunc`, `rndne`, `fract`, `abs`, `neg`,
    `f32_f64`, `cvt_f64_i32`, `cvt_f64_u32` and `ldexp` on every thread;
  - `min`/`max` on threads without a NaN operand (4010 checked, 86 skipped in `f64_v`;
    3776 checked, 320 skipped in `f64_s`);
  - all integer outputs: `frexp_exp` (finite inputs only), `cvt_f32_f64`, `cvt_i32_f64`,
    `cvt_u32_f64`, the compare mask, the class bits and `cmp_u_o`.
- **Not checked:** `div`, `sqrt`, `frexp_mant`, `rsq` and `rcp`. ACO builds these from f32
  approximations and Newton steps, which this model does not reproduce.

The same matcher on the bias-fix-only build fails 44 to 320 threads per checked operation. So the
model tells the two builds apart.

This accounts for the residual cases of the tolerance check: every in-range difference from
the f64 reference is the f32 result of truncated operands.

Representative cases from `f64_v` thread 148:
- **`add`:** `a = 0xc1e0000000100000` (-2147483648.5), `b = 0x41e0000000000000` (2^31).
  - In f64 the sum is -0.5.
  - `a` narrows to -2^31, because f32 keeps 24 bits of mantissa, so the lowered sum is 0.
- **`fract`:** of the same `a`, it is 0.5 in f64 and 0 after narrowing.

### 3. Tolerance check (execution, coarse)

`tolerance-matcher.patch` checks only threads whose operands and f64 reference are within
1e-30 to 1e30 in magnitude. Eligibility depends only on the operands and the reference, never
on the GPU result, so a wrong result cannot exclude itself.

Wrong in-range results per value operation of `f64_v`:

| build, fp64 off | |
|---|---|
| `473fa84` | about 3300 threads per operation |
| bias fix only | `sqrt` 1793, `fract` 1084, `add` 2 |
| `48b973a` | `fract` 1084, `add` 2 |

These counts were measured at open-test `eb1f2fe`, whose `f64_v` operands are identical to `905b8ce`'s.

### 4. SPIR-V (module validity only)

Shaders dumped with `GPU.dump_shaders=true`:
- **fp64 off:** none of the 12 modules has `OpCapability Float64` or `OpTypeFloat 64`, and all
  pass `spirv-val --target-env vulkan1.3`.
- **fp64 on:** the three f64 modules (`f64_v`, `f64_s`, `f64_pack`) have both.

### 5. Bit model (formulas only)

`bit-model.cpp` checks `PackedF64ToF32` against a reference that truncates and flushes below
`FLT_MIN`, over these inputs:
- the edge cases;
- every f64 exponent with six mantissa classes and both signs;
- 40M random f64 values.

It also checks `F32ToPackedF64` and the round trip on all 2^32 f32 values. All 9676
mismatches fall into the two documented classes:
- f64 denormals, which keep the internal encoding;
- NaNs whose payload is only in the low 29 bits, which become infinity.

The round trip is exact for every f32 value.

### 6. fp64 on (baseline regression only)

`shader_alu` passes 13/13. With fp64 support `recompiler.cpp` skips the pass, so this run only
shows that nothing else changed.

## Policies, not inherent limits

- **Rounding:** `PackedF64ToF32` truncates the mantissa. The immediate path
  (`static_cast<f32>`) rounds to nearest.
- **Below the f32 normal range:** f64 exponents 1 to 896 flush to a signed zero, even where an
  f32 denormal could represent the value. For example, a dynamic 2^-130 becomes 0.
- **Exponent 0:** it keeps the internal encoding of f32 denormals (`0x00000001` packs to
  `0x0000000020000000`, not `0x36a0000000000000`).
  - An f32 denormal therefore survives a round trip through a VGPR pair.
  - The stored f64 bits are not IEEE.
  - The round trip says nothing about arithmetic on such values.
