// SPDX-License-Identifier: GPL-2.0-or-later
// Real PM4 helpers, ordinary owned CPU memory; no Vulkan or guest execution.
#include "video_core/amdgpu/pm4_cmds.h"
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return 1000000; }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return 1000; }
} // namespace Libraries::Kernel
namespace Libraries::GnmDriver {
u32 PS4_SYSV_ABI sceGnmGetGpuCoreClockFrequency() { return 1000; }
} // namespace Libraries::GnmDriver
int main() {
  using namespace AmdGpu;
  alignas(8) u64 label{};
  const auto address = reinterpret_cast<std::uintptr_t>(&label);
  constexpr u32 value = 0x12345678;
  bool irq = false;
  bool store_before_irq = false;
  auto write = [&](void *dst, u64 data, u32 size) {
    std::memcpy(dst, &data, size);
  };
  auto interrupt = [&] {
    irq = true;
    store_before_irq = label == value;
  };

  PM4CmdEventWriteEop eop{.header = {PM4ItOpcode::EventWriteEop, 5},
                          .event_control = 0,
                          .address_lo = static_cast<u32>(address),
                          .data_control = 0,
                          .data_lo = value,
                          .data_hi = 0};
  eop.address_hi.Assign(static_cast<u32>(address >> 32));
  eop.data_sel.Assign(DataSelect::Data32Low);
  eop.int_sel.Assign(InterruptSelect::IrqWhenWriteConfirm);
  // Host-completion oracle remains false: helper has no such input/dependency.
  eop.SignalFence(write, interrupt);
  std::cout << "eop " << (label == value) << ' ' << irq << ' '
            << store_before_irq << '\n';
  label = 0;
  irq = false;
  eop.data_sel.Assign(DataSelect::None);
  eop.int_sel.Assign(InterruptSelect::IrqOnly);
  eop.SignalFence(write, interrupt);
  std::cout << "eop_irq_only " << label << ' ' << irq << '\n';

  PM4CmdEventWriteEos eos{.header = {PM4ItOpcode::EventWriteEos, 4},
                          .event_control = 0,
                          .address_lo = static_cast<u32>(address),
                          .cmd_info = 0,
                          .data = value};
  eos.address_hi.Assign(static_cast<u32>(address >> 32));
  eos.command.Assign(PM4CmdEventWriteEos::Command::SignalFence);
  eos.SignalFence(write);
  std::cout << "eos " << (label == value) << '\n';
  label = 0;
  eos.command.Assign(PM4CmdEventWriteEos::Command::GdsStore);
  eos.SignalFence(write);
  std::cout << "eos_gds_helper_no_store " << label << '\n';

  PM4CmdReleaseMem release{.header = {PM4ItOpcode::ReleaseMem, 6},
                           .dw1 = 0,
                           .dw2 = 0,
                           .address_lo = static_cast<u32>(address),
                           .address_hi = static_cast<u32>(address >> 32),
                           .data_lo = value,
                           .data_hi = 0};
  release.data_sel.Assign(DataSelect::Data32Low);
  release.int_sel.Assign(InterruptSelect::IrqWhenWriteConfirm);
  release.SignalFence(interrupt, [](VAddr, u16, u16) {});
  std::cout << "release_scalar " << (label == value) << ' ' << irq << ' '
            << store_before_irq << '\n';
  label = 0;
  irq = false;
  store_before_irq = false;
  bool transfer_recorded = false;
  release.data_sel.Assign(DataSelect::GdsMemStore);
  release.gds_index = 0;
  release.num_dw = 1;
  release.SignalFence(interrupt,
                      [&](VAddr, u16, u16) { transfer_recorded = true; });
  std::cout << "release_gds " << transfer_recorded << ' ' << irq << ' ' << label
            << '\n';
}
