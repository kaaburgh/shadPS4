// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>
#include "video_core/renderer_vulkan/uma_timeline_observer.h"
namespace Vulkan {
namespace {
std::mutex registry_mutex;
std::vector<UmaTimelineObserver*> observers;
} // namespace
UmaTimelineObserver::UmaTimelineObserver(vk::Device device_, vk::Semaphore semaphore_,
                                         uint64_t context_)
    : device{device_}, semaphore{semaphore_}, context{context_},
      thread{[this](std::stop_token stop) { Run(stop); }} {
    std::scoped_lock lock{registry_mutex};
    observers.push_back(this);
    UmaCensus::SetObserverShutdown(StopAll);
}
UmaTimelineObserver::~UmaTimelineObserver() {
    std::scoped_lock lock{registry_mutex};
    Stop();
    std::erase(observers, this);
}
void UmaTimelineObserver::Stop() {
    thread.request_stop();
    if (thread.joinable())
        thread.join();
    UmaCensus::Emit(UmaCensus::Kind::End, 0, head.load() - tail.load(), context);
}
void UmaTimelineObserver::StopAll() {
    std::scoped_lock lock{registry_mutex};
    for (auto* observer : observers)
        observer->Stop();
}
void UmaTimelineObserver::Submitted(uint64_t tick) {
    const auto position = head.load(std::memory_order_relaxed);
    if (position - tail.load(std::memory_order_acquire) == ticks.size()) {
        UmaCensus::Drop(); // Completion observation lost; no silent coalescing.
        return;
    }
    ticks[position % ticks.size()] = tick;
    head.store(position + 1, std::memory_order_release);
}
void UmaTimelineObserver::Run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        const auto position = tail.load(std::memory_order_relaxed);
        if (position == head.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        const auto tick = ticks[position % ticks.size()];
        const vk::SemaphoreWaitInfo info{
            .semaphoreCount = 1, .pSemaphores = &semaphore, .pValues = &tick};
        // No scheduler API, submit lock, queue submission, flush or barrier here.
        const auto result = device.waitSemaphores(info, 5000000);
        if (result == vk::Result::eTimeout)
            continue;
        UmaCensus::Emit(UmaCensus::Kind::Complete, 0, 0, context, 0, tick,
                        uint64_t(int64_t(result)));
        tail.store(position + 1, std::memory_order_release);
        if (result != vk::Result::eSuccess)
            break;
    }
}
} // namespace Vulkan
