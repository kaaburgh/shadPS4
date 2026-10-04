// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Common {
enum class CheckedWriteResult { Stored, Busy, Invalid, Cancelled };
// CP only: no mapping/owner lock is held while messages are pumped or slept on.
// A VMM mutator can hold its serialization lock while awaiting a CP download.
template <class Write, class Pump, class Await, class Stop>
CheckedWriteResult WriteWithCpProgress(Write write, Pump pump, Await await, Stop stop) {
    while (!stop()) {
        auto result = write();
        if (result != CheckedWriteResult::Busy)
            return result;
        pump();
        if (!stop())
            await();
    }
    return CheckedWriteResult::Cancelled;
}
} // namespace Common
