// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <random>

#include <gtest/gtest.h>

#include "core/libraries/audio/output_pacer.h"

using Libraries::AudioOut::OutputPacer;

namespace {

// 256 frames at 48 kHz, rounded as the backends do.
constexpr u64 Period = (1000000ULL * 256 + 48000 / 2) / 48000;
constexpr u64 MinSleep = OutputPacer::MinSleepThresholdUs;
constexpr u64 Start = 10'000'000;

OutputPacer MakePacer() {
    OutputPacer pacer;
    pacer.SetPeriod(Period);
    return pacer;
}

TEST(AudioOutputPacer, FirstOutputDoesNotWait) {
    auto pacer = MakePacer();
    EXPECT_EQ(pacer.OnOutput(Start), 0u);
}

TEST(AudioOutputPacer, WaitsForTheRestOfThePeriodWhenAhead) {
    auto pacer = MakePacer();
    pacer.OnOutput(Start);
    EXPECT_EQ(pacer.OnOutput(Start + 1000), Period - 1000 - MinSleep);
    // The deadline advanced by exactly one period.
    EXPECT_EQ(pacer.OnOutput(Start + Period), Period - MinSleep);
}

TEST(AudioOutputPacer, DoesNotWaitWhenOnTimeOrBehind) {
    auto pacer = MakePacer();
    pacer.OnOutput(Start);
    EXPECT_EQ(pacer.OnOutput(Start + Period), 0u);
    EXPECT_EQ(pacer.OnOutput(Start + 3 * Period), 0u);
    // Catching up: the deadline is still one period behind, so no wait yet.
    EXPECT_EQ(pacer.OnOutput(Start + 3 * Period + 100), 0u);
    EXPECT_EQ(pacer.OnOutput(Start + 3 * Period + 200), Period - 200 - MinSleep);
}

TEST(AudioOutputPacer, ResyncsWhenFarBehind) {
    auto pacer = MakePacer();
    pacer.OnOutput(Start);
    const u64 late = Start + 2'200'000;
    EXPECT_EQ(pacer.OnOutput(late), 0u);
    EXPECT_EQ(pacer.OnOutput(late + 1000), Period - 1000 - MinSleep);
}

TEST(AudioOutputPacer, BackwardClockJumpDoesNotSleep) {
    auto pacer = MakePacer();
    pacer.OnOutput(Start);
    pacer.OnOutput(Start + Period);
    // Same instant read on a core whose TSC is 2.2 s behind (shadps4-emu/shadPS4#5243).
    const u64 skewed = Start + Period - 2'200'000;
    EXPECT_EQ(pacer.OnOutput(skewed), 0u);
    // Pacing continues on the new timeline.
    EXPECT_EQ(pacer.OnOutput(skewed + 1000), Period - 1000 - MinSleep);
}

TEST(AudioOutputPacer, WrappedClockDoesNotSleep) {
    auto pacer = MakePacer();
    pacer.OnOutput(Start);
    // rdtsc below the process start counter wraps the unsigned process time.
    const u64 wrapped = std::numeric_limits<u64>::max() - 1000;
    EXPECT_EQ(pacer.OnOutput(wrapped), 0u);
    EXPECT_EQ(pacer.OnOutput(Start + Period), 0u);
}

TEST(AudioOutputPacer, SmallBackwardStepStillPaces) {
    auto pacer = MakePacer();
    pacer.OnOutput(Start);
    // Within two periods of the deadline the wait is honoured.
    EXPECT_EQ(pacer.OnOutput(Start - Period / 2), Period + Period / 2 - MinSleep);
}

// Output() reads the clock on whichever core the audio thread runs on. Replay real time
// advancing by each wait, with every read randomly skewed by up to +/-2.2 s.
TEST(AudioOutputPacer, CoreSkewNeverWaitsMoreThanTwoPeriods) {
    auto pacer = MakePacer();
    std::mt19937 rng(5243);
    std::uniform_int_distribution<int> core(0, 2);
    constexpr s64 Skew[] = {0, -2'200'000, 2'200'000};

    u64 real = Start + 3'000'000;
    u64 total_wait = 0;
    for (int i = 0; i < 10000; ++i) {
        const u64 now = real + Skew[core(rng)];
        const u64 wait = pacer.OnOutput(now);
        ASSERT_LE(wait, 2 * Period) << "call " << i;
        total_wait += wait;
        real += wait + 50;
    }
    // Some calls still pace.
    EXPECT_GT(total_wait, 0u);
}

TEST(AudioOutputPacer, SteadyClockKeepsPacing) {
    auto pacer = MakePacer();
    u64 now = Start;
    pacer.OnOutput(now);
    u64 total_wait = 0;
    constexpr int Calls = 1000;
    for (int i = 0; i < Calls; ++i) {
        now += 50; // The guest submits as fast as it can.
        const u64 wait = pacer.OnOutput(now);
        EXPECT_LE(wait, Period);
        total_wait += wait;
        now += wait;
    }
    // Paced to about one period per call.
    EXPECT_GE(now - Start, Calls * (Period - MinSleep));
    EXPECT_LE(now - Start, (Calls + 1) * Period);
    EXPECT_GT(total_wait, 0u);
}

} // namespace
