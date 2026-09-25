// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.h"

namespace e0b {

class Report;

// VK_EXT_map_memory_placed is newer than some distro headers; only its feature and property
// structures are needed, for reporting.
constexpr VkStructureType kSTypeMapPlacedFeatures = static_cast<VkStructureType>(1000272000);
constexpr VkStructureType kSTypeMapPlacedProperties = static_cast<VkStructureType>(1000272001);
struct MapPlacedFeatures {
    VkStructureType sType = kSTypeMapPlacedFeatures;
    void* pNext = nullptr;
    VkBool32 memoryMapPlaced = VK_FALSE;
    VkBool32 memoryMapRangePlaced = VK_FALSE;
    VkBool32 memoryUnmapReserve = VK_FALSE;
};
struct MapPlacedProperties {
    VkStructureType sType = kSTypeMapPlacedProperties;
    void* pNext = nullptr;
    VkDeviceSize minPlacedMemoryMapAlignment = 0;
};

struct HostBuffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* map = nullptr;
    VkDeviceAddress addr = 0;
    VkDeviceSize size = 0;
};

struct Arena {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceAddress addr = 0;
    VkMemoryRequirements reqs{};
    VkExternalMemoryHandleTypeFlags handles = 0;
    bool aliased = false;
};

struct Memory {
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint32_t type = UINT32_MAX;
    uint32_t import_bits = 0; // memory types the external handle allows
    VkResult result = VK_SUCCESS;
    std::string note;
    void* map = nullptr; // set by AllocateHostVisible
    explicit operator bool() const {
        return mem != VK_NULL_HANDLE;
    }
};

struct BindOp {
    VkDeviceSize arena_offset;
    VkDeviceSize size;
    VkDeviceMemory mem; // VK_NULL_HANDLE unbinds
    VkDeviceSize mem_offset;
};

struct GpuView {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceAddress addr = 0;
};

inline GpuView ArenaView(const Arena& a, VkDeviceSize off) {
    return {a.buf, off, a.addr + off};
}

enum class Path { Bda, Ssbo };
inline const char* PathName(Path p) {
    return p == Path::Bda ? "bda" : "ssbo";
}

struct SlotResult {
    uint32_t mismatches = 0;
    uint32_t first_bad = UINT32_MAX;
    uint32_t first_value = 0;
    uint32_t checked = 0;
};

class Context {
public:
    ~Context();

    // Instance-level.
    bool CreateInstance(bool validate, std::string* err);
    std::vector<VkPhysicalDevice> Devices() const;
    VkInstance Instance() const {
        return instance_;
    }
    uint32_t ValidationErrors() const {
        return validation_errors_;
    }

    // Device-level.
    bool CreateDevice(VkPhysicalDevice phys, const Options& opts, std::string* err);
    void DescribeDevice(Report& r) const;

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t qfam = UINT32_MAX;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memprops{};
    VkPhysicalDeviceDriverProperties driver{};

    bool ext_host = false;
    bool ext_fd = false;
    bool ext_dmabuf = false;
    bool ext_map_placed = false;
    bool f_sparse_binding = false;
    bool f_sparse_buffer = false;
    bool f_sparse_aliased = false;
    bool f_bda = false;
    bool f_int64 = false;
    bool f_timeline = false;
    VkDeviceSize min_host_ptr_align = 4096;
    VkDeviceSize ssbo_align = 16;
    bool force_type = false;
    bool device_lost = false;

    PFN_vkGetMemoryHostPointerPropertiesEXT GetHostPtrProps = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR GetFdProps = nullptr;
    PFN_vkGetMemoryFdKHR GetMemoryFd = nullptr;

    // Timeline shared by every submission and sparse bind.
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t NextTimelineValue() {
        return ++timeline_value_;
    }
    void WaitTimeline(uint64_t value);

    // Resources.
    HostBuffer CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void Destroy(HostBuffer& b);

    /// Creates a sparse arena like shadPS4's (SPARSE_BINDING|SPARSE_RESIDENCY, BDA) with the
    /// given external handle types; throws VkError on failure.
    Arena CreateArena(VkDeviceSize size, VkExternalMemoryHandleTypeFlags handles, bool aliased);
    void Destroy(Arena& a);

    /// Picks a memory type from bits, preferring HOST_COHERENT|HOST_CACHED.
    uint32_t PickHostType(uint32_t bits) const;
    uint32_t PickDeviceType(uint32_t bits) const;

    /// True when both ptr and size are multiples of the runtime minImportedHostPointerAlignment.
    bool HostImportAligned(const void* ptr, VkDeviceSize size) const {
        const VkDeviceSize a = min_host_ptr_align ? min_host_ptr_align : 1;
        return reinterpret_cast<uintptr_t>(ptr) % a == 0 && size % a == 0;
    }

    /// Imports host memory. `required_bits` are the types the consumer (sparse arena or plain
    /// buffer) accepts; an empty intersection is reported without allocating unless
    /// --force-type was given.
    Memory ImportHost(void* ptr, VkDeviceSize size, uint32_t required_bits, bool bda_flag);
    /// Imports a dma-buf fd; the fd is duplicated, the caller keeps ownership of `fd`.
    Memory ImportDmaBuf(int fd, VkDeviceSize size, uint32_t required_bits, bool bda_flag);
    Memory AllocateDevice(VkDeviceSize size, uint32_t required_bits);
    /// Allocates and maps ordinary HOST_VISIBLE memory from `required_bits`.
    Memory AllocateHostVisible(VkDeviceSize size, uint32_t required_bits);
    /// lavapipe implements sparse binds by mmap'ing its own memfd; imported or exported memory
    /// is silently not bound there (and freeing it afterwards can crash).
    bool IsLavapipe() const {
        return driver.driverID == VK_DRIVER_ID_MESA_LLVMPIPE;
    }
    /// Allocates host-visible memory exportable as `handle` and returns its fd in *out_fd.
    Memory AllocateExportable(VkDeviceSize size, uint32_t required_bits,
                              VkExternalMemoryHandleTypeFlagBits handle, int* out_fd);
    void Free(Memory& m);

    /// vkQueueBindSparse on the arena. Returns CPU time spent inside the call in µs; when
    /// host_wait is set, also waits for completion and adds the wait to *wall_us.
    double BindSparse(const Arena& a, const std::vector<BindOp>& ops,
                      std::optional<uint64_t> wait_value, std::optional<uint64_t> signal_value,
                      bool host_wait, double* wall_us = nullptr);

    // Pipelines (used by GpuBatch).
    VkPipelineLayout bda_layout = VK_NULL_HANDLE;
    VkPipelineLayout ssbo_layout = VK_NULL_HANDLE;
    VkPipeline bda_pipe = VK_NULL_HANDLE;
    VkPipeline ssbo_pipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout ssbo_dsl = VK_NULL_HANDLE;
    VkCommandPool cmd_pool = VK_NULL_HANDLE;

    void CheckResult(VkResult r, const char* what);

private:
    void CreatePipelines();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    uint32_t validation_errors_ = 0;
    uint64_t timeline_value_ = 0;
    VkFence bind_fence_ = VK_NULL_HANDLE;

    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);
};

/**
 * One command buffer of pattern operations. Records on construction, submits once (or
 * several times with Resubmit), and owns its result slots and descriptor pool.
 */
class GpuBatch {
public:
    explicit GpuBatch(Context& ctx, uint32_t max_slots = 64);
    ~GpuBatch();

    uint32_t Verify(Path p, GpuView src, uint32_t count, uint32_t seed);
    uint32_t NonZero(Path p, GpuView src, uint32_t count);
    void Write(Path p, GpuView dst, uint32_t count, uint32_t seed);
    void Barrier();
    void HostBarrier();

    void Submit(std::optional<uint64_t> wait_value = {}, std::optional<uint64_t> signal_value = {},
                bool host_wait = true);
    void Resubmit();
    void WaitHost();

    SlotResult Slot(uint32_t idx) const;

private:
    uint32_t Dispatch(Path p, GpuView src, GpuView dst, uint32_t count, uint32_t seed_in,
                      uint32_t seed_out, uint32_t mode, bool want_slot);

    Context& ctx_;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    HostBuffer slots_;
    VkDeviceSize slot_stride_ = 64;
    uint32_t max_slots_;
    uint32_t used_slots_ = 0;
    bool ended_ = false;
    bool pending_ = false;
};

} // namespace e0b
