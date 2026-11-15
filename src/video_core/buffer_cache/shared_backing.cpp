// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <string_view>

#ifdef __linux__
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include "common/alignment.h"
#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "core/memory.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/shared_backing.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace VideoCore {

namespace {

constexpr auto HostHandleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
constexpr auto DmaBufHandleType = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
constexpr u64 HostChunkBytes = 256_MB;
constexpr u64 DmaBufChunkBytes = 16_MB;
constexpr u64 DmaBufBudgetBytes = 64_MB;

} // Anonymous namespace

SharedBacking::SharedBacking(const Vulkan::Instance& instance_, Core::MemoryManager& memory)
    : instance{instance_}, requested{Core::IsUmaSharedBackingRequested()} {
    if (!requested) {
        return;
    }
    if (!instance.IsBufferDeviceAddressSupported()) {
        LOG_WARNING(Render_Vulkan, "UMA shared backing: BDA unavailable; backend=mirror");
        return;
    }

    auto& address_space = memory.GetAddressSpace();
    backing_base = address_space.BackingBase();
#ifdef __linux__
    if (instance.GetDriverID() == vk::DriverId::eMesaRadv) {
        if (!InitDmaBuf(address_space.GetBackingFd(), address_space.GetBackingSize())) {
            return;
        }
    } else
#endif
    {
        if (!instance.IsExternalMemoryHostSupported()) {
            LOG_WARNING(
                Render_Vulkan,
                "UMA shared backing: VK_EXT_external_memory_host unavailable; backend=mirror");
            return;
        }
        const auto alignment = instance.MinImportedHostPointerAlignment();
        if (alignment == 0 || HostChunkBytes % alignment != 0 ||
            reinterpret_cast<uintptr_t>(backing_base) % alignment != 0) {
            LOG_WARNING(Render_Vulkan,
                        "UMA shared backing: invalid host alignment {:#x}; backend=mirror",
                        alignment);
            return;
        }
        backend = Backend::HostPointer;
        chunk_size = HostChunkBytes;
        backing_size = Common::AlignDown(address_space.GetBackingSize(), alignment);
    }
    chunks.resize(Common::DivCeil(backing_size, chunk_size));
    enabled.store(true, std::memory_order_relaxed);
    LOG_INFO(
        Render_Vulkan, "UMA shared backing enabled: backend={}, chunk_bytes={}, budget_bytes={}",
        BackendName(), chunk_size, backend == Backend::DmaBuf ? DmaBufBudgetBytes : backing_size);
}

#ifdef __linux__
bool SharedBacking::InitDmaBuf(int fd, u64 size) {
    if (!instance.IsExternalMemoryDmaBufSupported()) {
        LOG_WARNING(Render_Vulkan,
                    "UMA shared backing: RADV DMA-BUF fd extensions unavailable; backend=mirror");
        return false;
    }
    const auto external = instance.GetPhysicalDevice()
                              .getExternalBufferProperties(vk::PhysicalDeviceExternalBufferInfo{
                                  .usage = AllFlags,
                                  .handleType = DmaBufHandleType,
                              })
                              .externalMemoryProperties;
    if (!(external.externalMemoryFeatures & vk::ExternalMemoryFeatureFlagBits::eImportable) ||
        !(external.compatibleHandleTypes & DmaBufHandleType)) {
        LOG_WARNING(Render_Vulkan,
                    "UMA shared backing: DMA-BUF buffer usage is not importable; backend=mirror");
        return false;
    }
    const int seals = fcntl(fd, F_GET_SEALS);
    if (seals < 0 || (seals & F_SEAL_SHRINK) == 0) {
        LOG_WARNING(Render_Vulkan, "UMA shared backing: memfd lacks F_SEAL_SHRINK; backend=mirror");
        return false;
    }
    s64 limit_mib{};
    std::ifstream limit{"/sys/module/udmabuf/parameters/size_limit_mb"};
    const long page_size = sysconf(_SC_PAGESIZE);
    if (!(limit >> limit_mib) || limit_mib <= 0 || page_size <= 0 ||
        page_size > static_cast<long>(DmaBufChunkBytes)) {
        LOG_WARNING(Render_Vulkan,
                    "UMA shared backing: udmabuf limit or page size unavailable; backend=mirror");
        return false;
    }
    const u64 kernel_limit = std::min<u64>(limit_mib, DmaBufChunkBytes / 1_MB) * 1_MB;
    const u64 runtime_limit =
        std::min({kernel_limit, instance.MaxBufferSize(), instance.MaxMemoryAllocationSize()});
    chunk_size = Common::AlignDown(runtime_limit, static_cast<u64>(page_size));
    if (chunk_size == 0) {
        LOG_WARNING(Render_Vulkan,
                    "UMA shared backing: udmabuf limit below a page; backend=mirror");
        return false;
    }
    backing_size = Common::AlignDown(size, static_cast<u64>(page_size));
    udmabuf_fd = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (udmabuf_fd < 0) {
        LOG_WARNING(Render_Vulkan, "UMA shared backing: open /dev/udmabuf: {}; backend=mirror",
                    strerror(errno));
        return false;
    }
    backing_fd = fd;
    dma_buf_dedicated_only =
        bool(external.externalMemoryFeatures & vk::ExternalMemoryFeatureFlagBits::eDedicatedOnly);
    backend = Backend::DmaBuf;
    return true;
}
#endif

SharedBacking::~SharedBacking() {
    Release();
}

void SharedBacking::Release() {
    // Each Chunk destroys its buffer before freeing the imported memory.
    const bool was_enabled = enabled.exchange(false, std::memory_order_relaxed);
    if (requested && (was_enabled || !chunks.empty())) {
        LOG_INFO(Render_Vulkan,
                 "UMA shared backing summary: backend={}, import_attempts={}, import_successes={}, "
                 "import_failures={}, live_chunks={}, live_bytes={}",
                 was_enabled ? BackendName() : "mirror", import_attempts, live_chunks,
                 import_attempts - live_chunks, live_chunks, imported_bytes);
    }
    chunks.clear();
    live_chunks = 0;
    imported_bytes = 0;
#ifdef __linux__
    if (udmabuf_fd >= 0) {
        close(std::exchange(udmabuf_fd, -1));
    }
#endif
    if (was_enabled) {
        LOG_INFO(Render_Vulkan, "UMA shared backing: imported buffers, memory and fd released");
    }
}

const char* SharedBacking::BackendName() const {
    switch (backend) {
    case Backend::HostPointer:
        return "host-pointer";
    case Backend::DmaBuf:
        return "dma-buf";
    case Backend::Mirror:
        return "mirror";
    }
    return "mirror";
}

std::optional<std::pair<Buffer*, u64>> SharedBacking::Lookup(PAddr phys_addr, u64 size) {
    if (!IsEnabled() || size == 0 || phys_addr >= backing_size || size > backing_size - phys_addr) {
        return std::nullopt;
    }
    const u64 index = phys_addr / chunk_size;
    if ((phys_addr + size - 1) / chunk_size != index) {
        return std::nullopt;
    }
    Chunk* chunk = GetChunk(index);
    if (!chunk) {
        return std::nullopt;
    }
    return std::make_pair(chunk->buffer.get(), phys_addr % chunk_size);
}

SharedBacking::Chunk* SharedBacking::GetChunk(u64 index) {
    auto& chunk = chunks[index];
    if (chunk.buffer) {
        return &chunk;
    }
    if (chunk.failed) {
        return nullptr;
    }
    ++import_attempts;
    if (!ImportChunk(index, chunk)) {
        chunk.failed = true;
        if (live_chunks == 0) {
            enabled.store(false, std::memory_order_relaxed);
            LOG_WARNING(Render_Vulkan, "UMA shared backing: first {} import failed; backend=mirror",
                        BackendName());
        }
        return nullptr;
    }
    return &chunk;
}

bool SharedBacking::ImportChunk(u64 index, Chunk& chunk) {
    const u64 base = index * chunk_size;
    const u64 size = std::min(chunk_size, backing_size - base);
    const auto fail = [&](std::string_view operation, std::string_view reason) {
        LOG_WARNING(Render_Vulkan, "UMA shared backing: backend={} physical={:#x} size={} {}: {}",
                    BackendName(), base, size, operation, reason);
        return false;
    };
    const bool dma_buf = backend == Backend::DmaBuf;
    if (dma_buf && size > DmaBufBudgetBytes - imported_bytes) {
        return fail("budget", "DMA-BUF pin budget exhausted");
    }
    const auto handle_type = dma_buf ? DmaBufHandleType : HostHandleType;
    const auto device = instance.GetDevice();
    const vk::ExternalMemoryBufferCreateInfo external_ci = {
        .handleTypes = handle_type,
    };
    const vk::BufferCreateInfo buffer_ci = {
        .pNext = &external_ci,
        .size = size,
        .usage = AllFlags,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    // Keep the buffer alive until it is transferred to Chunk. On failure it must be destroyed
    // before the bound allocation.
    vk::UniqueDeviceMemory memory;
    auto [create_result, buffer] = device.createBufferUnique(buffer_ci);
    if (create_result != vk::Result::eSuccess) {
        return fail("vkCreateBuffer", vk::to_string(create_result));
    }
    const auto requirements =
        device
            .getBufferMemoryRequirements2<vk::MemoryRequirements2, vk::MemoryDedicatedRequirements>(
                vk::BufferMemoryRequirementsInfo2{.buffer = *buffer});
    const auto reqs = requirements.get<vk::MemoryRequirements2>().memoryRequirements;
    const bool dedicated_required =
        requirements.get<vk::MemoryDedicatedRequirements>().requiresDedicatedAllocation;
    if (!dma_buf && dedicated_required) {
        return fail("buffer requirements", "host-pointer imports cannot be dedicated");
    }
    const bool dedicated_only = dma_buf && dma_buf_dedicated_only;
    if (reqs.size > size || ((dedicated_required || dedicated_only) && reqs.size != size)) {
        return fail("buffer requirements", fmt::format("required {} bytes", reqs.size));
    }

    int fd{-1};
#ifdef __linux__
    SCOPE_EXIT {
        if (fd >= 0) {
            close(fd);
        }
    };
#endif
    u32 external_type_bits{};
    if (dma_buf) {
#ifdef __linux__
        const udmabuf_create create = {
            .memfd = static_cast<u32>(backing_fd),
            .flags = UDMABUF_FLAGS_CLOEXEC,
            .offset = base,
            .size = size,
        };
        fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
        if (fd < 0) {
            return fail("UDMABUF_CREATE", strerror(errno));
        }
        const auto [result, props] = device.getMemoryFdPropertiesKHR(handle_type, fd);
        if (result != vk::Result::eSuccess) {
            return fail("vkGetMemoryFdPropertiesKHR", vk::to_string(result));
        }
        external_type_bits = props.memoryTypeBits;
#endif
    } else {
        const auto [result, props] =
            device.getMemoryHostPointerPropertiesEXT(handle_type, backing_base + base);
        if (result != vk::Result::eSuccess) {
            return fail("vkGetMemoryHostPointerPropertiesEXT", vk::to_string(result));
        }
        external_type_bits = props.memoryTypeBits;
    }
    // The CPU uses the canonical memfd or host mapping. Require coherent memory and leave
    // visibility and completion ordering to the existing E3 submit barriers and timeline.
    constexpr auto RequiredFlags =
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
    const auto& memory_types = instance.GetMemoryProperties().memoryTypes;
    u32 type_bits = external_type_bits & reqs.memoryTypeBits;
    for (u32 bits = type_bits; bits != 0; bits &= bits - 1) {
        const u32 type = std::countr_zero(bits);
        if ((memory_types[type].propertyFlags & RequiredFlags) != RequiredFlags) {
            type_bits &= ~(1U << type);
        }
    }
    if (type_bits == 0) {
        return fail("memory type",
                    fmt::format("no coherent intersection (external={:#x}, buffer={:#x})",
                                external_type_bits, reqs.memoryTypeBits));
    }
    const u32 memory_type = std::countr_zero(type_bits);

    const vk::MemoryDedicatedAllocateInfo dedicated = {
        .buffer = *buffer,
    };
    const vk::ImportMemoryFdInfoKHR fd_info = {
        .pNext = dedicated_required || dedicated_only ? &dedicated : nullptr,
        .handleType = DmaBufHandleType,
        .fd = fd,
    };
    const vk::ImportMemoryHostPointerInfoEXT host_info = {
        .handleType = HostHandleType,
        .pHostPointer = backing_base + base,
    };
    const vk::MemoryAllocateFlagsInfo flags_info = {
        .pNext =
            dma_buf ? static_cast<const void*>(&fd_info) : static_cast<const void*>(&host_info),
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::MemoryAllocateInfo alloc_info = {
        .pNext = &flags_info,
        .allocationSize = size,
        .memoryTypeIndex = memory_type,
    };
    auto [alloc_result, allocation] = device.allocateMemoryUnique(alloc_info);
    if (alloc_result != vk::Result::eSuccess) {
        return fail("vkAllocateMemory", vk::to_string(alloc_result));
    }
    fd = -1; // Successful Vulkan import consumed the DMA-BUF descriptor.
    memory = std::move(allocation);

    // Every buffer covers its entire chunk, so binding at offset zero meets any alignment.
    const auto bind_result = device.bindBufferMemory(*buffer, *memory, 0);
    if (bind_result != vk::Result::eSuccess) {
        return fail("vkBindBufferMemory", vk::to_string(bind_result));
    }
    const auto address = device.getBufferAddress(vk::BufferDeviceAddressInfo{.buffer = *buffer});
    if (address == 0) {
        return fail("vkGetBufferDeviceAddress", "zero device address");
    }

    chunk.buffer = std::make_unique<Buffer>(instance, base, size, std::move(buffer), address,
                                            fmt::format("Shared backing {:#x}", base));
    chunk.memory = std::move(memory);
    ++live_chunks;
    imported_bytes += size;
    LOG_INFO(Render_Vulkan,
             "UMA shared backing: imported backend={} physical={:#x} size={} memory_type={} "
             "external_types={:#x} buffer_types={:#x} live_chunks={} live_bytes={}",
             BackendName(), base, size, memory_type, external_type_bits, reqs.memoryTypeBits,
             live_chunks, imported_bytes);
    return true;
}

} // namespace VideoCore
