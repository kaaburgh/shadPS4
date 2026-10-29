// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Standalone model of the IR emitted by lower_fp64_to_fp32.cpp (kaaburgh/shadPS4#50 head 48b973a),
// with 32-bit unsigned wraparound and the same shifts/masks/selects.
#include <cfenv>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;

static u32 bfe(u32 base, u32 off, u32 cnt) { // BitFieldUExtract as in constant prop
    return (base >> off) & (cnt >= 32 ? ~0u : ((1u << cnt) - 1));
}
static u32 shl(u32 a, u32 b) { return static_cast<u32>(a << b); }

constexpr s32 F64ToF32Exp = +1023 - 127;
constexpr s32 F32ToF64Exp = +127 - 1023;

enum Variant { BASE, BIAS_ONLY, PR };

static u32 PackedF64ToF32(u32 lo, u32 hi, Variant v) {
    const u32 sign = bfe(hi, 31, 1);
    const u32 exp = bfe(hi, 20, 11);
    const u32 mantissa_hi = bfe(hi, 0, 20);
    const u32 mantissa_lo = bfe(lo, 29, 3);
    const u32 mantissa = shl(mantissa_hi, 3) | mantissa_lo;
    if (v == BASE || v == BIAS_ONLY) {
        const u32 e = v == BASE ? exp + u32(F64ToF32Exp) : exp - u32(F64ToF32Exp);
        const u32 exp_if_subnorm = exp == 0 ? 0u : e;
        const u32 exp_if_infnan = exp == 0x7ff ? 0xffu : exp_if_subnorm;
        return shl(sign, 31) | (shl(exp_if_infnan, 23) | mantissa);
    }
    const u32 exp_f32 = exp - u32(F64ToF32Exp);
    const bool is_infnan = exp == 0x7ff;
    const bool is_zero_or_denormal = exp == 0;
    const bool underflow = (s32(exp_f32) <= s32(0)) && !is_zero_or_denormal;
    const bool overflow = (s32(exp_f32) >= s32(0xff)) && !is_infnan;
    const u32 exp_if_subnorm = (is_zero_or_denormal || underflow) ? 0u : exp_f32;
    const u32 exp_if_infnan = (is_infnan || overflow) ? 0xffu : exp_if_subnorm;
    const u32 mantissa_in_range = (underflow || overflow) ? 0u : mantissa;
    return shl(sign, 31) | (shl(exp_if_infnan, 23) | mantissa_in_range);
}

static void F32ToPackedF64(u32 value, Variant v, u32& lo, u32& hi) {
    const u32 sign = bfe(value, 31, 1);
    const u32 exp = bfe(value, 23, 8);
    const u32 mantissa = bfe(value, 0, 23);
    const u32 mantissa_hi = bfe(mantissa, 3, 20);
    const u32 mantissa_lo = bfe(mantissa, 0, 3);
    const u32 e = v == BASE ? exp + u32(F32ToF64Exp) : exp - u32(F32ToF64Exp);
    const u32 exp_if_subnorm = exp == 0 ? 0u : e;
    const u32 exp_if_infnan = exp == 0xff ? 0x7ffu : exp_if_subnorm;
    lo = shl(mantissa_lo, 29);
    hi = shl(sign, 31) | (shl(exp_if_infnan, 20) | mantissa_hi);
}

static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static float u2f(u32 u) { float f; memcpy(&f, &u, 4); return f; }
static u64 d2u(double d) { u64 u; memcpy(&u, &d, 8); return u; }
static double u2d(u64 u) { double d; memcpy(&d, &u, 8); return d; }

// Reference for the PR's intended semantics: truncate toward zero, flush results below
// FLT_MIN to signed zero, |x| >= 2^128 -> inf, inf -> inf, NaN -> some NaN.
static u32 RefF64ToF32(u64 bits) {
    const double d = u2d(bits);
    if (std::isnan(d)) return 0x7fc00000u | (u32(bits >> 32) & 0x80000000u); // any NaN
    if (std::isinf(d)) return f2u(float(d));
    if (std::fabs(d) >= 0x1p128) return std::signbit(d) ? 0xff800000u : 0x7f800000u;
    std::fesetround(FE_TOWARDZERO);
    volatile double vd = d;
    float f = float(vd);
    std::fesetround(FE_TONEAREST);
    if (std::fabs(f) < 0x1p-126f) f = std::copysign(0.0f, f);
    return f2u(f);
}

static int failures = 0;
static void CheckF64(u64 bits, const char* label, bool verbose) {
    const u32 lo = u32(bits), hi = u32(bits >> 32);
    const u32 got = PackedF64ToF32(lo, hi, PR);
    const u32 ref = RefF64ToF32(bits);
    const bool nan_in = std::isnan(u2d(bits));
    const bool ok = nan_in ? std::isnan(u2f(got)) : got == ref;
    if (!ok || verbose) {
        printf("%-34s f64 %016" PRIx64 " (%-13.6g) -> PR %08x (%-13.6g) ref %08x %s | base %08x bias-only %08x\n",
               label, bits, u2d(bits), got, u2f(got), ref, ok ? "ok" : "MISMATCH",
               PackedF64ToF32(lo, hi, BASE), PackedF64ToF32(lo, hi, BIAS_ONLY));
    }
    if (!ok) ++failures;
}

int main() {
    // ---- f64 -> f32 edge classes
    struct { u64 b; const char* l; } edges[] = {
        {0x0000000000000000ull, "+0"},
        {0x8000000000000000ull, "-0"},
        {0x0000000000000001ull, "f64 min denormal"},
        {0x800fffffffffffffull, "-f64 max denormal"},
        {0x0010000000000000ull, "f64 min normal (exp 1)"},
        {0x3800000000000000ull, "exp 896, 2^-127"},
        {0x380fffffffffffffull, "exp 896 max mant"},
        {0xb800000000000000ull, "-exp 896"},
        {0x3810000000000000ull, "exp 897, FLT_MIN"},
        {0xb810000000000000ull, "-exp 897, -FLT_MIN"},
        {0x381fffffffffffffull, "exp 897 max mant"},
        {0x47e0000000000000ull, "exp 1150, 2^127"},
        {0x47efffffe0000000ull, "exp 1150, FLT_MAX"},
        {0x47efffffffffffffull, "exp 1150, max mant (>FLT_MAX)"},
        {0x47f0000000000000ull, "exp 1151, 2^128"},
        {0xc7f0000000000000ull, "-exp 1151"},
        {0x7fefffffffffffffull, "DBL_MAX"},
        {0x7ff0000000000000ull, "+inf"},
        {0xfff0000000000000ull, "-inf"},
        {0x7ff8000000000000ull, "qNaN canonical"},
        {0xfff8000000000000ull, "-qNaN"},
        {0x7ff0000100000000ull, "sNaN payload in hi"},
        {0x7ff0000000000001ull, "sNaN payload only low 29 bits"},
        {0x7ff000001fffffffull, "NaN payload low 29 bits (max)"},
        {0x7ff0000020000000ull, "NaN payload bit 29"},
        {0x1000000000000000ull, "2^-767 (ACO sqrt)"},
        {0x3ff8000000000000ull, "1.5"},
        {0x3fb999999999999aull, "0.1"},
        {0xbff0000000000000ull, "-1.0"},
    };
    for (auto& e : edges) CheckF64(e.b, e.l, true);

    // all exponents x few mantissas x both signs
    for (u64 s = 0; s < 2; ++s)
        for (u64 ex = 0; ex < 2048; ++ex)
            for (u64 m : {0ull, 1ull, 0x1fffffffull, 0x20000000ull, 0x8000000000000ull, 0xfffffffffffffull})
                CheckF64((s << 63) | (ex << 52) | m, "sweep", false);

    std::mt19937_64 rng(1234);
    for (int i = 0; i < 20000000; ++i) CheckF64(rng(), "random", false);
    // random finite doubles in the f32 range
    for (int i = 0; i < 20000000; ++i) {
        u64 ex = 860 + rng() % 330;
        CheckF64(((rng() & 1) << 63) | (ex << 52) | (rng() & 0xfffffffffffffull), "random-near-range", false);
    }
    printf("f64->f32 mismatches vs reference: %d\n", failures);

    // ---- f32 -> f64, exhaustive over all 2^32 f32 patterns
    u64 bad_conv = 0, bad_denorm = 0, rt_bad_pr = 0, rt_denorm_pr = 0, rt_bad_bias = 0, nan_to_inf = 0;
    u32 first_bad = 0, first_denorm = 0;
    bool have_bad = false;
    for (u64 x = 0; x <= 0xffffffffull; ++x) {
        const u32 v = u32(x);
        u32 lo, hi;
        F32ToPackedF64(v, PR, lo, hi);
        const u64 got = (u64(hi) << 32) | lo;
        const float f = u2f(v);
        const u32 exp = (v >> 23) & 0xff;
        const bool denorm = exp == 0 && (v & 0x7fffff) != 0;
        bool ok;
        if (std::isnan(f)) ok = std::isnan(u2d(got)) && ((got >> 29) & 0x7fffff) == (v & 0x7fffff);
        else ok = got == d2u(double(f));
        if (!ok) {
            if (denorm) { if (!bad_denorm++) first_denorm = v; }
            else { if (!have_bad) { first_bad = v; have_bad = true; } ++bad_conv; }
        }
        // round trip f32 -> packed f64 -> f32
        const u32 back = PackedF64ToF32(lo, hi, PR);
        if (back != v) { if (denorm) ++rt_denorm_pr; else ++rt_bad_pr; }
        u32 lo2, hi2;
        F32ToPackedF64(v, BIAS_ONLY, lo2, hi2);
        if (PackedF64ToF32(lo2, hi2, BIAS_ONLY) != v) ++rt_bad_bias;
        (void)nan_to_inf;
    }
    printf("f32->f64 exhaustive: non-denormal mismatches %" PRIu64 " (first %08x), denormal mismatches %" PRIu64
           " (first %08x)\n", bad_conv, first_bad, bad_denorm, first_denorm);
    {
        u32 lo, hi;
        F32ToPackedF64(0x00000001u, PR, lo, hi);
        printf("  e.g. f32 %08x (%g) -> f64 %08x%08x (%g), expected %016" PRIx64 "\n", 1u, u2f(1u), hi, lo,
               u2d((u64(hi) << 32) | lo), d2u(double(u2f(1u))));
        F32ToPackedF64(0x007fffffu, PR, lo, hi);
        printf("  e.g. f32 %08x (%g) -> f64 %08x%08x (%g), expected %016" PRIx64 "\n", 0x7fffffu, u2f(0x7fffffu), hi, lo,
               u2d((u64(hi) << 32) | lo), d2u(double(u2f(0x7fffffu))));
    }
    printf("round trip f32->f64->f32 (PR): non-denormal changed %" PRIu64 ", denormal changed %" PRIu64 "\n",
           rt_bad_pr, rt_denorm_pr);
    printf("round trip f32->f64->f32 (bias-only): changed %" PRIu64 "\n", rt_bad_bias);
    {
        u32 lo, hi;
        F32ToPackedF64(0x00400000u, PR, lo, hi);
        printf("  e.g. f32 00400000 -> PR round trip %08x, bias-only round trip %08x\n",
               PackedF64ToF32(lo, hi, PR), PackedF64ToF32(lo, hi, BIAS_ONLY));
    }

    // truncation vs RNE: how often PR result differs from a round-to-nearest conversion, in range
    u64 diff_rne = 0, n = 0;
    for (int i = 0; i < 10000000; ++i) {
        u64 ex = 897 + rng() % 254;
        u64 b = (ex << 52) | (rng() & 0xfffffffffffffull);
        u32 got = PackedF64ToF32(u32(b), u32(b >> 32), PR);
        u32 rne = f2u(float(u2d(b)));
        ++n;
        if (got != rne) { ++diff_rne; if (rne - got != 1) printf("RNE diff > 1 ulp: %016" PRIx64 "\n", b); }
    }
    printf("in-range f64: PR (truncate) differs from RNE in %" PRIu64 " / %" PRIu64 " (always by 1 ulp)\n", diff_rne, n);
    // inline constant 1/(2pi) and a GCN-literal-style f64 (low dword zero)
    u64 inv2pi = 0x3fc45f306dc9c882ull;
    printf("1/(2pi) f64 %016" PRIx64 ": truncated %08x, RNE %08x\n", inv2pi,
           PackedF64ToF32(u32(inv2pi), u32(inv2pi >> 32), PR), f2u(float(u2d(inv2pi))));
    return 0;
}
