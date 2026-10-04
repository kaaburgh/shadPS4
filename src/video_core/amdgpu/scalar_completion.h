// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/synchronization/guest_completion.h"

namespace AmdGpu {
inline VideoCore::Sync::ScalarData ScalarSelector(DataSelect data) {
    using VideoCore::Sync::ScalarData;
    switch (data) {
    case DataSelect::None:
        return ScalarData::None;
    case DataSelect::Data32Low:
        return ScalarData::Immediate32;
    case DataSelect::Data64:
        return ScalarData::Immediate64;
    case DataSelect::GpuClock64:
        return ScalarData::Clock;
    case DataSelect::PerfCounter:
        return ScalarData::Counter;
    default:
        UNREACHABLE_MSG("Non-scalar completion selector {}", u32(data));
    }
}
inline VideoCore::Sync::ScalarCompletion OwnScalar(const PM4CmdEventWriteEop& p) {
    if (!(p.int_sel == InterruptSelect::None || p.int_sel == InterruptSelect::IrqOnly ||
          p.int_sel == InterruptSelect::IrqWhenWriteConfirm) ||
        (p.int_sel == InterruptSelect::IrqOnly && p.data_sel != DataSelect::None))
        UNREACHABLE_MSG("Unsupported EOP scalar/IRQ combination");
    return {.family = VideoCore::Sync::ScalarFamily::Eop,
            .data = ScalarSelector(p.data_sel),
            .address = reinterpret_cast<VAddr>(p.Address<u32>()),
            .value = p.DataQWord(),
            .event_control = p.event_control,
            .interrupt = p.int_sel != InterruptSelect::None};
}
inline VideoCore::Sync::ScalarCompletion OwnScalar(const PM4CmdEventWriteEos& p) {
    if (p.command != PM4CmdEventWriteEos::Command::SignalFence)
        UNREACHABLE_MSG("Non-scalar EOS");
    return {.family = VideoCore::Sync::ScalarFamily::Eos,
            .data = VideoCore::Sync::ScalarData::Immediate32,
            .address = p.Address<VAddr>(),
            .value = p.DataDWord(),
            .event_control = p.event_control};
}
inline VideoCore::Sync::ScalarCompletion OwnScalar(const PM4CmdReleaseMem& p) {
    if (p.data_sel == DataSelect::None || p.data_sel == DataSelect::GdsMemStore ||
        !(p.int_sel == InterruptSelect::None || p.int_sel == InterruptSelect::IrqUndocumented ||
          p.int_sel == InterruptSelect::IrqWhenWriteConfirm))
        UNREACHABLE_MSG("Unsupported ReleaseMem scalar/IRQ combination");
    return {.family = VideoCore::Sync::ScalarFamily::Release,
            .data = ScalarSelector(p.data_sel),
            .address = p.Address<VAddr>(),
            .value = p.DataQWord(),
            .event_control = p.dw1,
            .interrupt = p.int_sel != InterruptSelect::None};
}
} // namespace AmdGpu
