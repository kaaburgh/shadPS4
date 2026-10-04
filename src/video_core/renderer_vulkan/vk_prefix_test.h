// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Opt-in isolated real-device adapter regression, before guest execution.
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {
inline void TestSubmittedPrefix(const Instance& instance, Scheduler& scheduler) {
    const auto device = instance.GetDevice();
    const vk::BufferCreateInfo buffer_info{.size = 12,
                                           .usage = vk::BufferUsageFlagBits::eTransferDst};
    auto [buffer_status, buffer] = device.createBufferUnique(buffer_info);
    ASSERT(buffer_status == vk::Result::eSuccess);
    const auto requirements = device.getBufferMemoryRequirements(*buffer);
    const auto& properties = instance.GetMemoryProperties();
    uint32_t type = properties.memoryTypeCount;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto needed =
            vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (properties.memoryTypes[i].propertyFlags & needed) == needed) {
            type = i;
            break;
        }
    }
    ASSERT(type < properties.memoryTypeCount);
    auto [memory_status, memory] = device.allocateMemoryUnique(
        vk::MemoryAllocateInfo{.allocationSize = requirements.size, .memoryTypeIndex = type});
    ASSERT(memory_status == vk::Result::eSuccess);
    Check(device.bindBufferMemory(*buffer, *memory, 0));
    const auto [map_status, mapped] = device.mapMemory(*memory, 0, requirements.size);
    ASSERT(map_status == vk::Result::eSuccess);
    auto* data = static_cast<uint32_t*>(mapped);
    data[0] = data[1] = data[2] = 0;
    auto record = [&](vk::CommandBuffer command, vk::DeviceSize offset, uint32_t value) {
        command.fillBuffer(*buffer, offset, 4, value);
        const vk::MemoryBarrier barrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                        .dstAccessMask = vk::AccessFlagBits::eHostRead};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                                vk::PipelineStageFlagBits::eHost, {}, barrier, {}, {});
    };
    record(scheduler.UploadCommandBuffer(), 8, 0xa0);
    record(scheduler.CommandBuffer(), 0, 0xa1);
    auto a = scheduler.FlushAndGetSubmittedTickForCurrentPrefix();
    ASSERT(a);
    record(scheduler.CommandBuffer(), 4, 0xb2); // B is recorded, not submitted.
    ASSERT(scheduler.CurrentTick() == a->Value() + 1);
    ASSERT(scheduler.WaitSubmitted(*a, {}) == VideoCore::Sync::WaitStatus::Completed);
    ASSERT(data[0] == 0xa1 && data[2] == 0xa0 && data[1] == 0);
    auto b = scheduler.FlushAndGetSubmittedTickForCurrentPrefix();
    ASSERT(b && b->Value() == a->Value() + 1);
    ASSERT(scheduler.WaitSubmitted(*b, {}) == VideoCore::Sync::WaitStatus::Completed);
    ASSERT(data[1] == 0xb2);
    // Existing Finish yields a typed proof. Admission must not add a second wait.
    auto proof = scheduler.FinishAndGetCompletedPrefix(100);
    int waits = 0, published = 0;
    VideoCore::Sync::GuestCompletionLane lane(
        scheduler.TimelineIdentity(),
        [&](auto&, auto) {
            ++waits;
            return VideoCore::Sync::WaitStatus::Failed;
        },
        [] {});
    ASSERT(lane.Enqueue(proof, {}));
    lane.Drain([&](auto&) { ++published; });
    ASSERT(published == 1 && waits == 0);
    lane.Stop();
    device.unmapMemory(*memory);
    // Unique buffer must be destroyed before its backing memory.
    device.destroyBuffer(*buffer);
    (void)buffer.release();
}
} // namespace Vulkan
