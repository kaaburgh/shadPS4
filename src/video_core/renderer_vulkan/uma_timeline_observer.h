// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <thread>
#include "common/uma_census.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
// One producer (scheduler), one consumer. Waits only on successful submitted ticks.
class UmaTimelineObserver {
public:
    UmaTimelineObserver(vk::Device device, vk::Semaphore semaphore, uint64_t context);
    ~UmaTimelineObserver();
    void Submitted(uint64_t tick);
    void Stop();
    static void StopAll();

private:
    void Run(std::stop_token stop);
    vk::Device device;
    vk::Semaphore semaphore;
    uint64_t context;
    std::array<uint64_t, 16384> ticks{};
    std::atomic<uint64_t> head{0}, tail{0};
    std::jthread thread;
};
} // namespace Vulkan
