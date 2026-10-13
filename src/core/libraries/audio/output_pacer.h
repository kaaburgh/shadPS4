// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Libraries::AudioOut {

// Paces audio output to one buffer period per Output() call.
//
// Output() runs with the port mutex held, so a wrong wait also blocks the guest. A real wait
// never exceeds one period: every call either resyncs to now + period, or sleeps until the
// previous deadline and advances it by one period. A longer wait means the process clock went
// backwards, e.g. rdtsc read on a core whose TSC lags the one that set the deadline, so the
// pacer resyncs instead of sleeping.
class OutputPacer {
public:
    static constexpr u64 MinSleepThresholdUs = 10;
    static constexpr u64 ResyncBehindUs = 100000; // Resync if >100ms behind

    void SetPeriod(u64 period) {
        period_us = period;
    }

    // Returns how long to sleep, in microseconds, before submitting the buffer at now_us.
    u64 OnOutput(u64 now_us) {
        if (next_output_time == 0) [[unlikely]] {
            Resync(now_us);
            return 0;
        }

        const s64 time_diff = static_cast<s64>(now_us - next_output_time);
        if (time_diff > static_cast<s64>(ResyncBehindUs)) [[unlikely]] {
            // Far behind schedule.
            Resync(now_us);
            return 0;
        }
        if (time_diff >= 0) {
            // On time or slightly behind: catch up without waiting.
            next_output_time += period_us;
            return 0;
        }

        const u64 time_to_wait = static_cast<u64>(-time_diff);
        if (time_to_wait > 2 * period_us) [[unlikely]] {
            // The clock went backwards.
            Resync(now_us);
            return 0;
        }
        next_output_time += period_us;
        return time_to_wait > MinSleepThresholdUs ? time_to_wait - MinSleepThresholdUs : 0;
    }

    // Restarts pacing from now_us; the next output is due one period later.
    void Resync(u64 now_us) {
        next_output_time = now_us + period_us;
    }

private:
    u64 period_us{0};
    u64 next_output_time{0};
};

} // namespace Libraries::AudioOut
