// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
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
 * on first use and exposed as plain buffers (E0b Plan B). Indexing by physical address
 * keeps an import valid across guest remaps, and two guest
 * aliases of the same physical page resolve to the same bytes.
 *
 * Opt-in with SHADPS4_UMA_SHARED_BACKING=1. Used from the CP thread only, except IsEnabled,
 * which any thread may call.
 */
class SharedBacking {
public:
    explicit SharedBacking(const Vulkan::Instance& instance, Core::MemoryManager& memory);
    ~SharedBacking();

    // Requires the CP to be stopped and all submitted GPU work to be retired.
    void Release();

    SharedBacking(const SharedBacking&) = delete;
    SharedBacking& operator=(const SharedBacking&) = delete;

    /// True when the opt-in is set and the device can import the backing. Turns false if the
    /// first import fails.
    [[nodiscard]] bool IsEnabled() const noexcept {
        return enabled.load(std::memory_order_relaxed);
    }

    /// Returns the buffer over physical range [phys_addr, phys_addr + size) and the offset of
    /// phys_addr in it, or nullopt when the range crosses a chunk or cannot be imported.
    std::optional<std::pair<Buffer*, u64>> Lookup(PAddr phys_addr, u64 size);

    // Inspect existing imports without triggering one.
    [[nodiscard]] bool OverlapsImported(PAddr phys_addr, u64 size) const;

private:
    struct Chunk {
        vk::UniqueDeviceMemory memory;
        std::unique_ptr<Buffer> buffer;
        bool failed{};
    };

    Chunk* GetChunk(u64 index);
    bool ImportChunk(u64 index, Chunk& chunk);

    enum class Backend { Mirror, HostPointer, DmaBuf };
    const char* BackendName() const;
#ifdef __linux__
    bool InitDmaBuf(int fd, u64 size);
    int backing_fd{-1};
    int udmabuf_fd{-1};
#endif

    const Vulkan::Instance& instance;
    Backend backend{Backend::Mirror};
    u8* backing_base{};
    u64 chunk_size{};
    u64 budget_bytes{};
    u64 imported_bytes{};
    u64 import_attempts{};
    u64 live_chunks{};
    u64 backing_size{};
    std::atomic<bool> enabled{};
    bool requested{};
    bool dma_buf_dedicated_only{};
    std::vector<Chunk> chunks;
};

} // namespace VideoCore
