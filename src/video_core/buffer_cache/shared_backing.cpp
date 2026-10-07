// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <cstdlib>
#include <string_view>

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/shared_backing.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace VideoCore {

namespace {

constexpr auto HostHandleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;

bool OptInRequested() {
    const char* value = std::getenv("SHADPS4_UMA_SHARED_BACKING");
    return value && std::string_view{value} == "1";
}

} // Anonymous namespace

SharedBacking::SharedBacking(const Vulkan::Instance& instance_, Core::MemoryManager& memory)
    : instance{instance_} {
    if (!OptInRequested()) {
        return;
    }
    if (!instance.IsExternalMemoryHostSupported()) {
        LOG_WARNING(Render_Vulkan, "UMA shared backing requested, but the device does not "
                                   "support VK_EXT_external_memory_host; using mirrors");
        return;
    }
    const auto alignment = instance.MinImportedHostPointerAlignment();
    if (CHUNK_SIZE % alignment != 0) {
        LOG_WARNING(Render_Vulkan,
                    "UMA shared backing: chunk size {:#x} is not a multiple of the host "
                    "import alignment {:#x}; using mirrors",
                    CHUNK_SIZE, alignment);
        return;
    }
    auto& address_space = memory.GetAddressSpace();
    backing_base = address_space.BackingBase();
    backing_size = Common::AlignDown(address_space.GetBackingSize(), alignment);
    chunks.resize(Common::DivCeil(backing_size, CHUNK_SIZE));
    enabled = true;
    LOG_INFO(Render_Vulkan,
             "UMA shared backing enabled: {:#x} bytes of guest backing in {} chunks of {:#x}",
             backing_size, chunks.size(), CHUNK_SIZE);
}

SharedBacking::~SharedBacking() {
    const auto device = instance.GetDevice();
    for (auto& chunk : chunks) {
        chunk.buffer.reset();
        if (chunk.memory) {
            device.freeMemory(chunk.memory);
        }
    }
}

std::optional<std::pair<Buffer*, u64>> SharedBacking::Lookup(PAddr phys_addr, u64 size) {
    if (!enabled || size == 0 || phys_addr >= backing_size || size > backing_size - phys_addr) {
        return std::nullopt;
    }
    const u64 index = phys_addr >> CHUNK_BITS;
    if (((phys_addr + size - 1) >> CHUNK_BITS) != index) {
        return std::nullopt;
    }
    Chunk* chunk = GetChunk(index);
    if (!chunk) {
        return std::nullopt;
    }
    return std::make_pair(chunk->buffer.get(), phys_addr - (index << CHUNK_BITS));
}

SharedBacking::Chunk* SharedBacking::GetChunk(u64 index) {
    auto& chunk = chunks[index];
    if (chunk.buffer) {
        return &chunk;
    }
    if (chunk.failed) {
        return nullptr;
    }
    chunk.failed = true;

    const u64 base = index << CHUNK_BITS;
    const u64 size = std::min(CHUNK_SIZE, backing_size - base);
    void* host_pointer = backing_base + base;
    const auto device = instance.GetDevice();

    const auto [props_result, host_props] =
        device.getMemoryHostPointerPropertiesEXT(HostHandleType, host_pointer);
    if (props_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "UMA shared backing: host pointer properties failed: {}",
                  vk::to_string(props_result));
        return nullptr;
    }
    const vk::ExternalMemoryBufferCreateInfo external_ci = {
        .handleTypes = HostHandleType,
    };
    const vk::BufferCreateInfo buffer_ci = {
        .pNext = &external_ci,
        .size = size,
        .usage = AllFlags,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const auto reqs = device
                          .getBufferMemoryRequirements(vk::DeviceBufferMemoryRequirements{
                              .pCreateInfo = &buffer_ci,
                          })
                          .memoryRequirements;
    // The host reads and writes the backing through its own mapping without flush or
    // invalidate, so the import must use a host-coherent type.
    constexpr auto RequiredFlags =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    const auto& memory_types = instance.GetMemoryProperties().memoryTypes;
    u32 type_bits = host_props.memoryTypeBits & reqs.memoryTypeBits;
    for (u32 bits = type_bits; bits != 0; bits &= bits - 1) {
        const u32 type = std::countr_zero(bits);
        if ((memory_types[type].propertyFlags & RequiredFlags) != RequiredFlags) {
            type_bits &= ~(1U << type);
        }
    }
    if (type_bits == 0) {
        LOG_ERROR(Render_Vulkan,
                  "UMA shared backing: no host-coherent memory type for imported buffers (host "
                  "{:#x}, buffer {:#x})",
                  host_props.memoryTypeBits, reqs.memoryTypeBits);
        return nullptr;
    }
    const u32 memory_type = std::countr_zero(type_bits);

    const vk::ImportMemoryHostPointerInfoEXT import_info = {
        .handleType = HostHandleType,
        .pHostPointer = host_pointer,
    };
    const vk::MemoryAllocateFlagsInfo flags_info = {
        .pNext = &import_info,
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::MemoryAllocateInfo alloc_info = {
        .pNext = &flags_info,
        .allocationSize = size,
        .memoryTypeIndex = memory_type,
    };
    const auto [alloc_result, memory] = device.allocateMemory(alloc_info);
    if (alloc_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "UMA shared backing: importing chunk {:#x} failed: {}", base,
                  vk::to_string(alloc_result));
        return nullptr;
    }
    chunk.memory = memory;
    chunk.buffer = std::make_unique<Buffer>(instance, base, size, memory,
                                            fmt::format("Shared backing {:#x}", base));
    chunk.failed = false;
    LOG_INFO(Render_Vulkan,
             "UMA shared backing: imported physical {:#x}..{:#x} as memory type {} (host types "
             "{:#x}, buffer types {:#x})",
             base, base + size, memory_type, host_props.memoryTypeBits, reqs.memoryTypeBits);
    return &chunk;
}

} // namespace VideoCore
