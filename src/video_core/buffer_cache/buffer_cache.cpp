// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <magic_enum/magic_enum.hpp>

#include "common/alignment.h"
#include "core/debug_state.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/buffer_cache/region_definitions.h"
#include "video_core/buffer_cache/shared_backing.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    // Each arena is a sparse buffer, so its size must not exceed maxBufferSize and must fit in
    // the device's sparse address space. Keep 4 GiB arenas where the device allows them and
    // otherwise use the largest power of two that fits, leaving room for two arenas (a request
    // may span an arena boundary). For example, lavapipe reports maxBufferSize = 4 GiB - 1 and
    // sparseAddressSpaceSize = 2 GiB, so it gets 1 GiB arenas.
    const u64 arena_limit =
        std::min<u64>(instance.MaxBufferSize(), instance.SparseAddressSpaceSize() / 2);
    arena_page_bits =
        std::clamp<u64>(std::bit_width(arena_limit) - 1, MIN_ARENA_PAGE_BITS, MAX_ARENA_PAGE_BITS);
    arena_page_size = u64{1} << arena_page_bits;
    address_space.resize(u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits));
    if (arena_page_bits < MAX_ARENA_PAGE_BITS) {
        LOG_INFO(Render,
                 "Using {:#x}-byte sparse buffer arenas (maxBufferSize {:#x}, "
                 "sparseAddressSpaceSize {:#x})",
                 arena_page_size, instance.MaxBufferSize(), instance.SparseAddressSpaceSize());
    }

    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    scheduler.Census(UmaCensus::Kind::BlockSize, 0, block_size);
    if (UmaCensus::Enabled()) {
        UmaCensus::Metadata("readbacks_mode", std::to_string(EmulatorSettings.GetReadbacksMode()));
        UmaCensus::Metadata("userfaultfd_requested",
                            EmulatorSettings.IsUserfaultfdTracking() ? "true" : "false");
        UmaCensus::Metadata("directMemoryAccess",
                            EmulatorSettings.IsDirectMemoryAccessEnabled() ? "true" : "false");
    }
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();

    shared_backing = std::make_unique<SharedBacking>(instance, *memory);
    if (shared_backing->IsEnabled()) {
        LOG_INFO(Render, "UMA shared backing uses {:#x}-byte blocks", block_size);
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * address_space.size()) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * address_space.size());
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() = default;

void BufferCache::TickFrame() {
    if (std::exchange(fault_process_pending, false)) {
        fault_manager->ProcessFaultBuffer();
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    UmaCensus::Emit(UmaCensus::Kind::Tracker, device_addr, size, 0, 0, 0, 1);
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    const auto flush_request = [this, device_addr, size, is_write] {
        const u32 first_block = device_addr >> block_shift;
        const u32 last_block = (device_addr + size - 1) >> block_shift;
        const auto* arena = GetArena(first_block, last_block);

        // GPU-modified ranges come as many small scattered islands,
        // so the download is widened to a window around the request
        constexpr u64 WindowSize = 512_KB;
        const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
        const VAddr window_start =
            std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
        const VAddr window_end = std::min<VAddr>(
            std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
    };
    if (assume_locks) {
        flush_request();
    } else {
        liverpool->SendCommand<true>(std::move(flush_request));
    }
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    boost::container::small_vector<vk::BufferCopy, 1> copies;
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    if (total_size_bytes == 0) {
        return;
    }
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    scheduler.Census(UmaCensus::Kind::Readback, device_addr, size, 2, total_size_bytes);
    scheduler.Finish(2);
    UmaCensus::OriginScope census_origin{2};

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size, false);
}

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    // UMA E3: guest memory the GPU can use directly needs no stream copy, upload or download.
    if (shared_backing->IsEnabled()) {
        if (const auto shared =
                ObtainSharedBuffer(device_addr, size, is_texel_buffer && !is_written)) {
            scheduler.Census(UmaCensus::Kind::Buffer, device_addr, size, is_written ? 3 : 1,
                             is_texel_buffer, 3);
            return *shared;
        }
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size) &&
        !IsRegionShared(device_addr, size)) {
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        scheduler.Census(UmaCensus::Kind::Buffer, device_addr, size, 1, is_texel_buffer, 1);
        scheduler.Census(UmaCensus::Kind::Snapshot, device_addr, size, 1);
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    if (shared_backing->IsEnabled()) {
        DemoteSharedBlocks(first_block << block_shift, (last_block + 1) << block_shift);
    }
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (is_texel_buffer && !is_written) {
        SynchronizeMemoryFromImage(arena, arena->Offset(device_addr), device_addr, size);
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    scheduler.Census(UmaCensus::Kind::Buffer, device_addr, size, is_written ? 3 : 1,
                     is_texel_buffer, 0);
    return {arena, arena->Offset(device_addr)};
}

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    // A shared region may have GPU writes that are recorded but not yet executed, so the image
    // must be filled by a copy on the GPU timeline rather than from guest memory now.
    if (shared_backing->IsEnabled()) {
        if (const auto shared = ObtainSharedBuffer(device_addr, size, false)) {
            return *shared;
        }
    }
    if (IsRegionGpuModified(device_addr, size) || IsRegionShared(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    scheduler.Census(UmaCensus::Kind::Buffer, device_addr, size, 1, 1, 2);
    scheduler.Census(UmaCensus::Kind::Snapshot, device_addr, size, 2);
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::SynchronizeDmaBuffers() {
    scheduler.Census(UmaCensus::Kind::DmaSet, 0, 0, 3);
    fault_process_pending = true;
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        scheduler.Census(UmaCensus::Kind::DmaRange, device_addr, size, 3);
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    ASSERT_MSG(last_page - first_page <= 1,
               "Buffer request cannot span more than two VA arena pages");

    const auto* first_arena = address_space[first_page];
    const auto* last_arena = address_space[last_page];
    if (first_arena == last_arena) {
        if (!first_arena) {
            const u64 base_block = Common::AlignDownPow2<u64>(first_block, blocks_per_arena_page);
            const u64 num_pages = last_page - first_page + 1;
            const auto* new_arena =
                &arenas.emplace_back(instance, base_block << block_shift,
                                     num_pages << arena_page_bits, MemoryType::Sparse);
            address_space[first_page] = new_arena;
            address_space[last_page] = new_arena;
        }
        return address_space[first_page];
    }

    LOG_WARNING(Render, "Migrating arena");

    const u64 first_addr = first_arena ? first_arena->cpu_addr : (first_page << arena_page_bits);
    const u64 first_size = first_arena ? first_arena->size_bytes : arena_page_size;
    const u64 last_size = last_arena ? last_arena->size_bytes : arena_page_size;

    const u64 base_block = first_addr >> block_shift;
    const u64 total_size = first_size + last_size;
    const u64 end_block = (first_addr + total_size) >> block_shift;
    auto* new_arena = &arenas.emplace_back(instance, first_addr, total_size, MemoryType::Sparse);
    auto* bind = BindsForArena(new_arena);
    resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
        const u64 start = std::max(base_block, backing.start);
        const u64 end = std::min(end_block, backing.end);
        bind->binds.push_back(vk::SparseMemoryBind{
            .resourceOffset = (start - base_block) << block_shift,
            .size = (end - start) << block_shift,
            .memory = backing.memory,
            .memoryOffset = (backing.offset + start - backing.start) << block_shift,
        });
    });

    u64 base_page = first_addr >> arena_page_bits;
    for (u32 page = 0; page < (first_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    base_page = last_page;
    for (u32 page = 0; page < (last_size >> arena_page_bits); ++page) {
        address_space[base_page + page] = new_arena;
    }
    return new_arena;
}

void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    u32 resident_blocks{};
    IntervalList bind_ranges;
    resident_ranges.ForEachGap(first_block, last_block + 1, [&](u64 start, u64 end) {
        resident_blocks += end - start;
        bind_ranges.Add({start, end});
    });

    if (bind_ranges.Empty()) {
        return;
    }

    const vk::MemoryAllocateInfo alloc_info = {
        .allocationSize = resident_blocks << block_shift,
        .memoryTypeIndex = arena_memory_type_index,
    };
    const auto device_memory = Vulkan::Check(instance.GetDevice().allocateMemory(alloc_info));

    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const auto staging =
        staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress), MemoryType::HostUncached);

    u64 memory_offset{};
    ArenaBinds* binds = BindsForArena(arena);
    auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
    u64 offset = staging.offset;
    for (const auto& range : bind_ranges) {
        Backing backing;
        backing.start = range.start;
        backing.end = range.end;
        backing.memory = device_memory;
        backing.offset = memory_offset >> block_shift;
        scheduler.Census(UmaCensus::Kind::Resident, backing.start << block_shift,
                         (backing.end - backing.start) << block_shift, 1);
        resident_ranges.Add(backing);

        LOG_INFO(Render, "Making range start={}, end={} resident", backing.start, backing.end);

        const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
            .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
            .size = (range.end - range.start) << block_shift,
            .memory = device_memory,
            .memoryOffset = memory_offset,
        });
        memory_offset += bind.size;

        for (u32 block = 0; block < bind.size; block += block_size) {
            *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
        }
        const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
        copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
        offset += copy_size;
    }

    staging.Flush();
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    memory_tracker->ForEachUploadRange(device_addr, size, is_written, [&](u64 addr, u64 size) {
        copies.emplace_back(total_size_bytes, addr, size);
        total_size_bytes += size;
    });
    if (!copies.empty()) {
        const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
        for (auto& copy : copies) {
            scheduler.Census(UmaCensus::Kind::Snapshot, copy.dstOffset, copy.size, 3);
            memory->CopySparseMemory(copy.dstOffset, staging.mapped + copy.srcOffset, copy.size);
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        staging.Flush();
        runtime.CopyBuffer(staging.buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, arena->Offset(device_addr), device_addr, size);
    }
    return false;
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* buffer, u64 buffer_offset,
                                             VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(buffer, buffer_offset, size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        return false;
    }
    Image& image = texture_cache.GetImage(image_id);
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (buffer_offset + mip_info.offset + mip_info.size > buffer->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, buffer, buffer_offset);
    return true;
}

bool BufferCache::IsSharedBackingEnabled() const noexcept {
    return shared_backing->IsEnabled();
}

bool BufferCache::IsRegionShared(VAddr addr, u64 size) const {
    return shared_backing->IsEnabled() && shared_ranges.Intersects(addr, size);
}

std::optional<std::pair<const Buffer*, u64>> BufferCache::ObtainSharedBuffer(VAddr device_addr,
                                                                             u64 size,
                                                                             bool is_texel_read) {
    // Shared state is kept per block, the granularity of the BDA page table, so that buffer
    // descriptors and DMA accesses of one block always see the same memory.
    const VAddr block_start = Common::AlignDown(device_addr, block_size);
    const VAddr block_end = Common::AlignUp(device_addr + size, block_size);
    const u64 first_block = block_start >> block_shift;
    const u64 end_block = block_end >> block_shift;
    bool has_mirror = false;
    resident_ranges.ForEachInRange(first_block, end_block,
                                   [&has_mirror](const Backing&) { has_mirror = true; });
    if (has_mirror) {
        // A block that already has arena memory stays mirrored until the guest unmaps it.
        return std::nullopt;
    }
    if (is_texel_read && (texture_cache.IsMeta(device_addr) ||
                          texture_cache.FindImageFromRange(device_addr, size))) {
        // Reading an image as a texel buffer copies the image into the buffer. Keep that in
        // the mirror instead of overwriting guest memory.
        return std::nullopt;
    }
    const u64 block_bytes = block_end - block_start;
    const auto phys_addr = memory->GetContiguousBacking(block_start, block_bytes);
    if (!phys_addr) {
        return std::nullopt;
    }
    const auto shared = shared_backing->Lookup(*phys_addr, block_bytes);
    if (!shared) {
        return std::nullopt;
    }
    const auto [buffer, block_offset] = *shared;
    if (!shared_ranges.Contains(block_start, block_bytes)) {
        MapSharedBlocks(block_start, block_end, buffer->BufferDeviceAddress() + block_offset);
    }
    shared_backing_used = true;
    return std::make_pair(buffer, block_offset + (device_addr - block_start));
}

void BufferCache::MapSharedBlocks(VAddr block_start, VAddr block_end,
                                  vk::DeviceAddress device_addr) {
    boost::container::small_vector<vk::DeviceAddress, 64> entries;
    for (VAddr block = block_start; block < block_end; block += block_size) {
        entries.push_back(device_addr + (block - block_start));
    }
    WriteBdaEntries(block_start >> block_shift, entries);
    shared_ranges.Add(block_start, block_end - block_start);
}

void BufferCache::DemoteSharedBlocks(VAddr block_start, VAddr block_end) {
    const u64 size = block_end - block_start;
    if (!shared_ranges.Intersects(block_start, size)) {
        return;
    }
    LOG_WARNING(Render, "UMA shared backing: {:#x}..{:#x} falls back to the mirror", block_start,
                block_end);
    // The mirror uploads from guest memory, which must first receive the writes of work that
    // used these blocks through the shared backing. EnsureResident then rewrites their BDA
    // page table entries.
    scheduler.Finish();
    shared_ranges.Subtract(block_start, size);
    memory_tracker->MarkRegionAsCpuModified(block_start, size);
}

void BufferCache::UnmapMemory(VAddr device_addr, u64 size) {
    if (!shared_backing->IsEnabled() || size == 0) {
        return;
    }
    liverpool->SendCommand<true>([this, device_addr, size] {
        // The next use of an unmapped block resolves its guest memory again, so a remap to
        // other physical memory is seen.
        const VAddr block_start = Common::AlignDown(device_addr, block_size);
        const VAddr block_end = Common::AlignUp(device_addr + size, block_size);
        boost::container::small_vector<std::pair<VAddr, VAddr>, 4> ranges;
        shared_ranges.ForEachInRange(
            block_start, block_end - block_start,
            [&ranges](VAddr start, VAddr end) { ranges.emplace_back(start, end); });
        for (const auto& [start, end] : ranges) {
            const std::vector<vk::DeviceAddress> entries((end - start) >> block_shift, 0);
            WriteBdaEntries(start >> block_shift, entries);
        }
        shared_ranges.Subtract(block_start, block_end - block_start);
    });
}

void BufferCache::WriteBdaEntries(u64 first_block, std::span<const vk::DeviceAddress> entries) {
    const u64 bytes = entries.size_bytes();
    const auto staging = staging_pool.Request(bytes, MemoryType::HostUncached);
    std::memcpy(staging.mapped, entries.data(), bytes);
    staging.Flush();
    const vk::BufferCopy copy = {
        .srcOffset = staging.offset,
        .dstOffset = first_block * sizeof(vk::DeviceAddress),
        .size = bytes,
    };
    runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), {&copy, 1});
}

void BufferCache::RecordSharedBackingVisibility() {
    if (!std::exchange(shared_backing_used, false)) {
        return;
    }
    // Guest completion is published once this submit's timeline value is reached; the guest
    // then reads its memory directly, so device writes must be available to the host by then.
    const vk::MemoryBarrier2 barrier = {
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead | vk::AccessFlagBits2::eHostWrite,
    };
    scheduler.EndRendering();
    scheduler.CommandBuffer().pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    scheduler.Census(UmaCensus::Kind::ArenaBind, 0, pending_binds.size(), signal_tick,
                     uint64_t(int64_t(submit_result)), scheduler.CensusSubmittingTick());
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    pending_binds.clear();
}

} // namespace VideoCore
