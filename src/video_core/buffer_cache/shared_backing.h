// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {
class Instance;
}

namespace Core {
class MemoryManager;
}

namespace VideoCore {

struct Buffer;

/**
 * UMA E3: the guest physical backing imported into Vulkan, so that a buffer bound to it is the
 * guest memory itself instead of a device-local mirror.
 *
 * Fixed-size chunks of the canonical backing mapping (AddressSpace::BackingBase) are imported
 * with VK_EXT_external_memory_host on first use and exposed as plain buffers ("plan B" of the
 * E0b probe: neither lavapipe nor the tested NVIDIA driver accepts imported memory in a sparse
 * arena). Indexing by physical address keeps an import valid across guest remaps, and two guest
 * aliases of the same physical page resolve to the same bytes.
 *
 * Opt-in with SHADPS4_UMA_SHARED_BACKING=1. Not thread safe: used from the CP thread only.
 */
class SharedBacking {
public:
    static constexpr u64 CHUNK_BITS = 28;
    static constexpr u64 CHUNK_SIZE = 1ULL << CHUNK_BITS;

    explicit SharedBacking(const Vulkan::Instance& instance, Core::MemoryManager& memory);
    ~SharedBacking();

    SharedBacking(const SharedBacking&) = delete;
    SharedBacking& operator=(const SharedBacking&) = delete;

    /// True when the opt-in is set and the device can import the backing.
    [[nodiscard]] bool IsEnabled() const noexcept {
        return enabled;
    }

    /// Returns the buffer over physical range [phys_addr, phys_addr + size) and the offset of
    /// phys_addr in it, or nullopt when the range crosses a chunk or cannot be imported.
    std::optional<std::pair<Buffer*, u64>> Lookup(PAddr phys_addr, u64 size);

private:
    struct Chunk {
        vk::DeviceMemory memory;
        std::unique_ptr<Buffer> buffer;
        bool failed{};
    };

    Chunk* GetChunk(u64 index);

    const Vulkan::Instance& instance;
    u8* backing_base{};
    u64 backing_size{};
    bool enabled{};
    std::vector<Chunk> chunks;
};

} // namespace VideoCore
