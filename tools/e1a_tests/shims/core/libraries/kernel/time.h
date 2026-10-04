// Test-only clock declarations, copied signatures; no packet behavior is
// mocked.
#pragma once
#include "common/types.h"
#include <chrono>
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency();
u64 PS4_SYSV_ABI sceKernelReadTsc();
} // namespace Libraries::Kernel
