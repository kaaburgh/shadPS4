// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "report.h"
#include "spirv_shaders.h"
#include "vk_ctx.h"

namespace e0b {

constexpr uint64_t kWaitTimeoutNs = 20ull * 1000 * 1000 * 1000;

// Same usage as shadPS4's buffer_cache.cpp ARENA_USAGE.
constexpr VkBufferUsageFlags kArenaUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

struct BdaPush {
    uint64_t src;
    uint64_t dst;
    uint64_t slot;
    uint32_t count;
    uint32_t seed_in;
    uint32_t seed_out;
    uint32_t mode;
};
static_assert(sizeof(BdaPush) == 40);

struct SsboPush {
    uint32_t count;
    uint32_t seed_in;
    uint32_t seed_out;
    uint32_t mode;
};

static bool HasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(), [&](const VkExtensionProperties& e) {
        return std::strcmp(e.extensionName, name) == 0;
    });
}

Context::~Context() {
    if (device) {
        vkDeviceWaitIdle(device);
        if (bda_pipe) {
            vkDestroyPipeline(device, bda_pipe, nullptr);
        }
        if (ssbo_pipe) {
            vkDestroyPipeline(device, ssbo_pipe, nullptr);
        }
        if (bda_layout) {
            vkDestroyPipelineLayout(device, bda_layout, nullptr);
        }
        if (ssbo_layout) {
            vkDestroyPipelineLayout(device, ssbo_layout, nullptr);
        }
        if (ssbo_dsl) {
            vkDestroyDescriptorSetLayout(device, ssbo_dsl, nullptr);
        }
        if (cmd_pool) {
            vkDestroyCommandPool(device, cmd_pool, nullptr);
        }
        if (bind_fence_) {
            vkDestroyFence(device, bind_fence_, nullptr);
        }
        if (timeline) {
            vkDestroySemaphore(device, timeline, nullptr);
        }
        vkDestroyDevice(device, nullptr);
    }
    if (messenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) {
            destroy(instance_, messenger_, nullptr);
        }
    }
    if (instance_) {
        vkDestroyInstance(instance_, nullptr);
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL Context::DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    auto* self = static_cast<Context*>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (++self->validation_errors_ <= 50) {
            std::printf("  [validation error] %s\n", data->pMessage);
        }
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::printf("  [validation warning] %s\n", data->pMessage);
    }
    return VK_FALSE;
}

bool Context::CreateInstance(bool validate, std::string* err) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
    uint32_t nl = 0;
    vkEnumerateInstanceLayerProperties(&nl, nullptr);
    std::vector<VkLayerProperties> layers(nl);
    vkEnumerateInstanceLayerProperties(&nl, layers.data());

    std::vector<const char*> enabled_exts;
    std::vector<const char*> enabled_layers;
    const bool have_debug = HasExtension(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validate) {
        const bool have_layer = std::any_of(layers.begin(), layers.end(), [](const auto& l) {
            return std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        if (have_layer) {
            enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        } else {
            std::printf("  # --validate: VK_LAYER_KHRONOS_validation not installed, continuing "
                        "without it\n");
        }
        if (have_debug) {
            enabled_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "e0b_uma_probe";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(enabled_exts.size());
    ci.ppEnabledExtensionNames = enabled_exts.data();
    ci.enabledLayerCount = static_cast<uint32_t>(enabled_layers.size());
    ci.ppEnabledLayerNames = enabled_layers.data();
    const VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        *err = std::string("vkCreateInstance: ") + ResultName(r);
        return false;
    }
    if (validate && have_debug) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mci{
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = DebugCallback;
        mci.pUserData = this;
        if (create) {
            create(instance_, &mci, nullptr, &messenger_);
        }
    }
    return true;
}

std::vector<VkPhysicalDevice> Context::Devices() const {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> v(n);
    vkEnumeratePhysicalDevices(instance_, &n, v.data());
    return v;
}

bool Context::CreateDevice(VkPhysicalDevice p, const Options& opts, std::string* err) {
    phys = p;
    force_type = opts.force_type;
    vkGetPhysicalDeviceProperties(phys, &props);
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    if (VK_API_VERSION_MINOR(props.apiVersion) < 2 && VK_API_VERSION_MAJOR(props.apiVersion) == 1) {
        *err = "device does not support Vulkan 1.2";
        return false;
    }
    ssbo_align = std::max<VkDeviceSize>(4, props.limits.minStorageBufferOffsetAlignment);

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
    ext_host = HasExtension(exts, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    ext_fd = HasExtension(exts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    ext_dmabuf = ext_fd && HasExtension(exts, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    ext_map_placed = HasExtension(exts, "VK_EXT_map_memory_placed");

    // Properties.
    driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    props2.pNext = &driver;
    if (ext_host) {
        driver.pNext = &host_props;
    }
    vkGetPhysicalDeviceProperties2(phys, &props2);
    if (ext_host) {
        min_host_ptr_align = host_props.minImportedHostPointerAlignment;
    }

    // Features.
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &f2);
    f_sparse_binding = f2.features.sparseBinding;
    f_sparse_buffer = f2.features.sparseResidencyBuffer;
    f_sparse_aliased = f2.features.sparseResidencyAliased;
    f_int64 = f2.features.shaderInt64;
    f_bda = f12.bufferDeviceAddress;
    f_timeline = f12.timelineSemaphore;
    if (!f_timeline) {
        *err = "timelineSemaphore is required";
        return false;
    }

    // Queue: compute + sparse binding, like shadPS4's single graphics queue.
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf.data());
    for (uint32_t i = 0; i < nq && qfam == UINT32_MAX; ++i) {
        if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) &&
            (qf[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)) {
            qfam = i;
        }
    }
    if (qfam == UINT32_MAX) {
        f_sparse_binding = false; // no queue can execute binds
        for (uint32_t i = 0; i < nq && qfam == UINT32_MAX; ++i) {
            if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                qfam = i;
            }
        }
    }
    if (qfam == UINT32_MAX) {
        *err = "no compute queue";
        return false;
    }

    // Device.
    VkPhysicalDeviceFeatures2 en2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features en12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    en2.pNext = &en12;
    en2.features.sparseBinding = f_sparse_binding;
    en2.features.sparseResidencyBuffer = f_sparse_binding && f_sparse_buffer;
    en2.features.sparseResidencyAliased = f_sparse_binding && f_sparse_aliased;
    en2.features.shaderInt64 = f_int64;
    en12.bufferDeviceAddress = f_bda;
    en12.timelineSemaphore = VK_TRUE;

    std::vector<const char*> dev_exts;
    if (ext_host) {
        dev_exts.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    }
    if (ext_fd) {
        dev_exts.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    }
    if (ext_dmabuf) {
        dev_exts.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &en2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_exts.size());
    dci.ppEnabledExtensionNames = dev_exts.data();
    const VkResult r = vkCreateDevice(phys, &dci, nullptr, &device);
    if (r != VK_SUCCESS) {
        *err = std::string("vkCreateDevice: ") + ResultName(r);
        return false;
    }
    vkGetDeviceQueue(device, qfam, 0, &queue);
    if (ext_host) {
        GetHostPtrProps = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
            vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT"));
    }
    if (ext_fd) {
        GetFdProps = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
            vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
        GetMemoryFd =
            reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
    }

    VkSemaphoreTypeCreateInfo stci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    stci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &stci;
    E0B_CHECK(vkCreateSemaphore(device, &sci, nullptr, &timeline));

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qfam;
    E0B_CHECK(vkCreateCommandPool(device, &pci, nullptr, &cmd_pool));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    E0B_CHECK(vkCreateFence(device, &fci, nullptr, &bind_fence_));

    CreatePipelines();
    return true;
}

void Context::DescribeDevice(Report& r) const {
    r.Fact("device.name", props.deviceName);
    r.Fact("device.vendor_id", Hex(props.vendorID));
    r.Fact("device.device_id", Hex(props.deviceID));
    r.Fact("device.type", props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete"
                          : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                              ? "integrated"
                          : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "cpu"
                                                                            : "other");
    r.Fact("device.api_version",
           Sprintf("%u.%u.%u", VK_API_VERSION_MAJOR(props.apiVersion),
                   VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion)));
    r.Fact("device.driver_version_raw", Hex(props.driverVersion));
    r.Fact("device.driver_name", driver.driverName);
    r.Fact("device.driver_info", driver.driverInfo);
    r.Fact("device.queue_family", std::to_string(qfam));
    r.Fact("ext.VK_EXT_external_memory_host", ext_host ? "yes" : "no");
    r.Fact("ext.VK_KHR_external_memory_fd", ext_fd ? "yes" : "no");
    r.Fact("ext.VK_EXT_external_memory_dma_buf", ext_dmabuf ? "yes" : "no");
    r.Fact("ext.VK_EXT_map_memory_placed", ext_map_placed ? "yes" : "no");
    r.Fact("feature.sparseBinding(+queue)", f_sparse_binding ? "yes" : "no");
    r.Fact("feature.sparseResidencyBuffer", f_sparse_buffer ? "yes" : "no");
    r.Fact("feature.sparseResidencyAliased", f_sparse_aliased ? "yes" : "no");
    r.Fact("feature.bufferDeviceAddress", f_bda ? "yes" : "no");
    r.Fact("feature.shaderInt64", f_int64 ? "yes" : "no");
    r.Fact("prop.minImportedHostPointerAlignment", Hex(min_host_ptr_align));
    r.Fact("prop.minStorageBufferOffsetAlignment",
           Hex(props.limits.minStorageBufferOffsetAlignment));
    r.Fact("prop.maxMemoryAllocationCount", std::to_string(props.limits.maxMemoryAllocationCount));
    r.Fact("prop.nonCoherentAtomSize", Hex(props.limits.nonCoherentAtomSize));
    r.Fact("prop.sparseAddressSpaceSize", Hex(props.limits.sparseAddressSpaceSize));
    r.Fact("prop.residencyNonResidentStrict",
           props.sparseProperties.residencyNonResidentStrict ? "yes" : "no");

    if (ext_map_placed) {
        MapPlacedFeatures mf;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &mf;
        vkGetPhysicalDeviceFeatures2(phys, &f2);
        MapPlacedProperties mp;
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &mp;
        vkGetPhysicalDeviceProperties2(phys, &p2);
        r.Fact("map_placed.memoryMapPlaced", mf.memoryMapPlaced ? "yes" : "no");
        r.Fact("map_placed.memoryMapRangePlaced", mf.memoryMapRangePlaced ? "yes" : "no");
        r.Fact("map_placed.memoryUnmapReserve", mf.memoryUnmapReserve ? "yes" : "no");
        r.Fact("map_placed.minPlacedMemoryMapAlignment", Hex(mp.minPlacedMemoryMapAlignment));
    }

    for (uint32_t h = 0; h < memprops.memoryHeapCount; ++h) {
        const auto& heap = memprops.memoryHeaps[h];
        r.Fact(Sprintf("heap[%u]", h),
               Sprintf("%llu MiB%s", static_cast<unsigned long long>(heap.size / MiB),
                       (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : ""));
    }
    for (uint32_t t = 0; t < memprops.memoryTypeCount; ++t) {
        const auto f = memprops.memoryTypes[t].propertyFlags;
        std::string s = Sprintf("heap %u:", memprops.memoryTypes[t].heapIndex);
        if (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            s += " DEVICE_LOCAL";
        if (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            s += " HOST_VISIBLE";
        if (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            s += " HOST_COHERENT";
        if (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
            s += " HOST_CACHED";
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
            s += " LAZY";
        if (f & VK_MEMORY_PROPERTY_PROTECTED_BIT)
            s += " PROTECTED";
        r.Fact(Sprintf("memtype[%u]", t), s);
    }
}

void Context::CheckResult(VkResult r, const char* what) {
    if (r == VK_ERROR_DEVICE_LOST) {
        device_lost = true;
    }
    if (r != VK_SUCCESS) {
        throw VkError(r, std::string(what) + " -> " + ResultName(r));
    }
}

void Context::WaitTimeline(uint64_t value) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline;
    wi.pValues = &value;
    CheckResult(vkWaitSemaphores(device, &wi, kWaitTimeoutNs), "vkWaitSemaphores");
}

void Context::CreatePipelines() {
    auto make_module = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = bytes;
        ci.pCode = code;
        VkShaderModule m;
        E0B_CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
        return m;
    };
    auto make_pipe = [&](VkShaderModule m, VkPipelineLayout layout) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = layout;
        VkPipeline p;
        E0B_CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
        return p;
    };

    if (f_bda && f_int64) {
        VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BdaPush)};
        VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        lci.pushConstantRangeCount = 1;
        lci.pPushConstantRanges = &pcr;
        E0B_CHECK(vkCreatePipelineLayout(device, &lci, nullptr, &bda_layout));
        VkShaderModule m = make_module(pattern_bda_spv, sizeof(pattern_bda_spv));
        bda_pipe = make_pipe(m, bda_layout);
        vkDestroyShaderModule(device, m, nullptr);
    }

    VkDescriptorSetLayoutBinding b[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = 3;
    dci.pBindings = b;
    E0B_CHECK(vkCreateDescriptorSetLayout(device, &dci, nullptr, &ssbo_dsl));
    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SsboPush)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &ssbo_dsl;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    E0B_CHECK(vkCreatePipelineLayout(device, &lci, nullptr, &ssbo_layout));
    VkShaderModule m = make_module(pattern_ssbo_spv, sizeof(pattern_ssbo_spv));
    ssbo_pipe = make_pipe(m, ssbo_layout);
    vkDestroyShaderModule(device, m, nullptr);
}

uint32_t Context::PickHostType(uint32_t bits) const {
    uint32_t best = UINT32_MAX;
    int best_score = -1;
    for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) {
            continue;
        }
        const auto f = memprops.memoryTypes[i].propertyFlags;
        const int score = ((f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? 4 : 0) +
                          ((f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 2 : 0) +
                          ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? 1 : 0);
        if (score > best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

uint32_t Context::PickDeviceType(uint32_t bits) const {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i) {
        if (!(bits & (1u << i))) {
            continue;
        }
        const auto f = memprops.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            !(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            return i;
        }
        if (fallback == UINT32_MAX) {
            fallback = i;
        }
    }
    return fallback;
}

HostBuffer Context::CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    HostBuffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage | (f_bda ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
    E0B_CHECK(vkCreateBuffer(device, &bci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buf, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i) {
        const auto f = memprops.memoryTypes[i].propertyFlags;
        if ((req.memoryTypeBits & (1u << i)) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) {
        throw VkError(VK_ERROR_FEATURE_NOT_PRESENT,
                      "no host-visible coherent type for result buffer");
    }
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = f_bda ? &fi : nullptr;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    E0B_CHECK(vkAllocateMemory(device, &ai, nullptr, &b.mem));
    E0B_CHECK(vkBindBufferMemory(device, b.buf, b.mem, 0));
    E0B_CHECK(vkMapMemory(device, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
    if (f_bda) {
        VkBufferDeviceAddressInfo ai2{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai2.buffer = b.buf;
        b.addr = vkGetBufferDeviceAddress(device, &ai2);
    }
    return b;
}

void Context::Destroy(HostBuffer& b) {
    if (b.buf) {
        vkDestroyBuffer(device, b.buf, nullptr);
    }
    if (b.mem) {
        vkFreeMemory(device, b.mem, nullptr);
    }
    b = {};
}

Arena Context::CreateArena(VkDeviceSize size, VkExternalMemoryHandleTypeFlags handles,
                           bool aliased) {
    if (!f_sparse_binding || !f_sparse_buffer) {
        throw Unsupported(
            "sparseBinding/sparseResidencyBuffer (or a sparse-capable queue) not available");
    }
    if (aliased && !f_sparse_aliased) {
        throw Unsupported("sparseResidencyAliased not supported");
    }
    Arena a;
    a.size = size;
    a.handles = handles;
    a.aliased = aliased;
    VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ext.handleTypes = handles;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = handles ? &ext : nullptr;
    bci.flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT |
                (aliased ? VK_BUFFER_CREATE_SPARSE_ALIASED_BIT : 0);
    bci.size = size;
    bci.usage = kArenaUsage;
    if (!f_bda) {
        bci.usage &= ~VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    E0B_CHECK(vkCreateBuffer(device, &bci, nullptr, &a.buf));
    vkGetBufferMemoryRequirements(device, a.buf, &a.reqs);
    if (f_bda) {
        VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        ai.buffer = a.buf;
        a.addr = vkGetBufferDeviceAddress(device, &ai);
    }
    return a;
}

void Context::Destroy(Arena& a) {
    if (a.buf) {
        vkDestroyBuffer(device, a.buf, nullptr);
    }
    a = {};
}

Memory Context::ImportHost(void* ptr, VkDeviceSize size, uint32_t required_bits, bool bda_flag) {
    Memory m;
    m.size = size;
    if (!ext_host || !GetHostPtrProps) {
        m.result = VK_ERROR_EXTENSION_NOT_PRESENT;
        m.note = "VK_EXT_external_memory_host not available";
        return m;
    }
    VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    m.result =
        GetHostPtrProps(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, ptr, &hp);
    if (m.result != VK_SUCCESS) {
        m.note = std::string("vkGetMemoryHostPointerPropertiesEXT -> ") + ResultName(m.result);
        return m;
    }
    m.import_bits = hp.memoryTypeBits;
    uint32_t bits = hp.memoryTypeBits & required_bits;
    if (!bits) {
        if (!force_type) {
            m.result = VK_ERROR_FEATURE_NOT_PRESENT;
            m.note = "empty memory type intersection: import " + TypeBits(hp.memoryTypeBits) +
                     " vs required " + TypeBits(required_bits);
            return m;
        }
        bits = hp.memoryTypeBits;
        m.note = "FORCED memory type outside the resource's memoryTypeBits (invalid usage)";
    }
    m.type = PickHostType(bits);
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = ptr;
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (bda_flag && f_bda) {
        imp.pNext = &fi;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = size;
    ai.memoryTypeIndex = m.type;
    m.result = vkAllocateMemory(device, &ai, nullptr, &m.mem);
    if (m.result != VK_SUCCESS) {
        m.mem = VK_NULL_HANDLE;
        m.note = std::string("vkAllocateMemory(host import) -> ") + ResultName(m.result);
    }
    if (m.result == VK_ERROR_DEVICE_LOST) {
        device_lost = true;
    }
    return m;
}

Memory Context::ImportDmaBuf(int fd, VkDeviceSize size, uint32_t required_bits, bool bda_flag) {
    Memory m;
    m.size = size;
    if (!ext_dmabuf || !GetFdProps) {
        m.result = VK_ERROR_EXTENSION_NOT_PRESENT;
        m.note = "VK_EXT_external_memory_dma_buf not available";
        return m;
    }
    VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    m.result = GetFdProps(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
    if (m.result != VK_SUCCESS) {
        m.note = std::string("vkGetMemoryFdPropertiesKHR -> ") + ResultName(m.result);
        return m;
    }
    m.import_bits = fp.memoryTypeBits;
    uint32_t bits = fp.memoryTypeBits & required_bits;
    if (!bits) {
        if (!force_type) {
            m.result = VK_ERROR_FEATURE_NOT_PRESENT;
            m.note = "empty memory type intersection: import " + TypeBits(fp.memoryTypeBits) +
                     " vs required " + TypeBits(required_bits);
            return m;
        }
        bits = fp.memoryTypeBits;
        m.note = "FORCED memory type outside the resource's memoryTypeBits (invalid usage)";
    }
    m.type = PickHostType(bits);
    const int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dup_fd < 0) {
        m.result = VK_ERROR_TOO_MANY_OBJECTS;
        m.note = "dup(dma-buf fd) failed";
        return m;
    }
    VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    imp.fd = dup_fd;
    VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    if (bda_flag && f_bda) {
        imp.pNext = &fi;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = size;
    ai.memoryTypeIndex = m.type;
    m.result = vkAllocateMemory(device, &ai, nullptr, &m.mem);
    if (m.result != VK_SUCCESS) {
        close(dup_fd); // ownership only transfers on success
        m.mem = VK_NULL_HANDLE;
        m.note = std::string("vkAllocateMemory(dma-buf import) -> ") + ResultName(m.result);
    }
    if (m.result == VK_ERROR_DEVICE_LOST) {
        device_lost = true;
    }
    return m;
}

Memory Context::AllocateDevice(VkDeviceSize size, uint32_t required_bits) {
    Memory m;
    m.size = size;
    m.type = PickDeviceType(required_bits);
    if (m.type == UINT32_MAX) {
        m.result = VK_ERROR_FEATURE_NOT_PRESENT;
        m.note = "no memory type";
        return m;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = size;
    ai.memoryTypeIndex = m.type;
    m.result = vkAllocateMemory(device, &ai, nullptr, &m.mem);
    if (m.result != VK_SUCCESS) {
        m.mem = VK_NULL_HANDLE;
        m.note = std::string("vkAllocateMemory -> ") + ResultName(m.result);
    }
    return m;
}

Memory Context::AllocateHostVisible(VkDeviceSize size, uint32_t required_bits) {
    Memory m;
    m.size = size;
    uint32_t host_bits = 0;
    for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i) {
        if (memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            host_bits |= 1u << i;
        }
    }
    m.type = PickHostType(required_bits & host_bits);
    if (m.type == UINT32_MAX) {
        m.result = VK_ERROR_FEATURE_NOT_PRESENT;
        m.note =
            "no HOST_VISIBLE memory type in the arena's memoryTypeBits " + TypeBits(required_bits);
        return m;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = size;
    ai.memoryTypeIndex = m.type;
    m.result = vkAllocateMemory(device, &ai, nullptr, &m.mem);
    if (m.result != VK_SUCCESS) {
        m.mem = VK_NULL_HANDLE;
        m.note = std::string("vkAllocateMemory(host visible) -> ") + ResultName(m.result);
        return m;
    }
    m.result = vkMapMemory(device, m.mem, 0, VK_WHOLE_SIZE, 0, &m.map);
    if (m.result != VK_SUCCESS) {
        m.note = std::string("vkMapMemory -> ") + ResultName(m.result);
        Free(m);
    }
    return m;
}

Memory Context::AllocateExportable(VkDeviceSize size, uint32_t required_bits,
                                   VkExternalMemoryHandleTypeFlagBits handle, int* out_fd) {
    Memory m;
    m.size = size;
    *out_fd = -1;
    uint32_t host_bits = 0;
    for (uint32_t i = 0; i < memprops.memoryTypeCount; ++i) {
        if (memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
            host_bits |= 1u << i;
        }
    }
    m.type = PickHostType(required_bits & host_bits);
    if (m.type == UINT32_MAX || !GetMemoryFd) {
        m.result = VK_ERROR_FEATURE_NOT_PRESENT;
        m.note = !GetMemoryFd ? "vkGetMemoryFdKHR not available"
                              : "no HOST_VISIBLE type in " + TypeBits(required_bits);
        return m;
    }
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.handleTypes = handle;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &exp;
    ai.allocationSize = size;
    ai.memoryTypeIndex = m.type;
    m.result = vkAllocateMemory(device, &ai, nullptr, &m.mem);
    if (m.result != VK_SUCCESS) {
        m.mem = VK_NULL_HANDLE;
        m.note = std::string("vkAllocateMemory(exportable) -> ") + ResultName(m.result);
        return m;
    }
    VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gi.memory = m.mem;
    gi.handleType = handle;
    m.result = GetMemoryFd(device, &gi, out_fd);
    if (m.result != VK_SUCCESS) {
        m.note = std::string("vkGetMemoryFdKHR -> ") + ResultName(m.result);
        Free(m);
    }
    return m;
}

void Context::Free(Memory& m) {
    if (m.mem) {
        vkFreeMemory(device, m.mem, nullptr);
    }
    m.mem = VK_NULL_HANDLE;
}

double Context::BindSparse(const Arena& a, const std::vector<BindOp>& ops,
                           std::optional<uint64_t> wait_value, std::optional<uint64_t> signal_value,
                           bool host_wait, double* wall_us) {
    std::vector<VkSparseMemoryBind> binds;
    binds.reserve(ops.size());
    for (const auto& op : ops) {
        binds.push_back({op.arena_offset, op.size, op.mem, op.mem_offset, 0});
    }
    VkSparseBufferMemoryBindInfo bi{a.buf, static_cast<uint32_t>(binds.size()), binds.data()};

    const uint64_t wv = wait_value.value_or(0);
    const uint64_t sv = signal_value.value_or(0);
    VkTimelineSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tl.waitSemaphoreValueCount = wait_value ? 1 : 0;
    tl.pWaitSemaphoreValues = &wv;
    tl.signalSemaphoreValueCount = signal_value ? 1 : 0;
    tl.pSignalSemaphoreValues = &sv;

    VkBindSparseInfo info{VK_STRUCTURE_TYPE_BIND_SPARSE_INFO};
    info.pNext = &tl;
    info.waitSemaphoreCount = wait_value ? 1 : 0;
    info.pWaitSemaphores = &timeline;
    info.signalSemaphoreCount = signal_value ? 1 : 0;
    info.pSignalSemaphores = &timeline;
    info.bufferBindCount = binds.empty() ? 0 : 1;
    info.pBufferBinds = &bi;

    if (host_wait) {
        CheckResult(vkResetFences(device, 1, &bind_fence_), "vkResetFences");
    }
    const double t0 = NowUs();
    CheckResult(vkQueueBindSparse(queue, 1, &info, host_wait ? bind_fence_ : VK_NULL_HANDLE),
                "vkQueueBindSparse");
    const double t1 = NowUs();
    if (host_wait) {
        CheckResult(vkWaitForFences(device, 1, &bind_fence_, VK_TRUE, kWaitTimeoutNs),
                    "vkWaitForFences(bind)");
    }
    if (wall_us) {
        *wall_us = NowUs() - t0;
    }
    return t1 - t0;
}

// ---------------------------------------------------------------------------------------------

GpuBatch::GpuBatch(Context& ctx, uint32_t max_slots) : ctx_(ctx), max_slots_(max_slots + 1) {
    slot_stride_ = std::max<VkDeviceSize>(64, ctx_.ssbo_align);
    slots_ = ctx_.CreateHostBuffer(slot_stride_ * max_slots_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for (uint32_t i = 0; i < max_slots_; ++i) {
        auto* s = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(slots_.map) + i * slot_stride_);
        s[0] = 0;
        s[1] = UINT32_MAX;
        s[2] = 0;
        s[3] = 0;
    }

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = ctx_.cmd_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    E0B_CHECK(vkAllocateCommandBuffers(ctx_.device, &ai, &cmd_));

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * max_slots_ * 2};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = max_slots_ * 2;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &ps;
    E0B_CHECK(vkCreateDescriptorPool(ctx_.device, &pci, nullptr, &dpool_));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    E0B_CHECK(vkCreateFence(ctx_.device, &fci, nullptr, &fence_));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    E0B_CHECK(vkBeginCommandBuffer(cmd_, &bi));
    // Host writes done before submission are visible to the device; state it explicitly.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         1, &mb, 0, nullptr, 0, nullptr);
}

GpuBatch::~GpuBatch() {
    if (pending_ && !ctx_.device_lost) {
        vkWaitForFences(ctx_.device, 1, &fence_, VK_TRUE, kWaitTimeoutNs);
    }
    if (cmd_) {
        vkFreeCommandBuffers(ctx_.device, ctx_.cmd_pool, 1, &cmd_);
    }
    if (dpool_) {
        vkDestroyDescriptorPool(ctx_.device, dpool_, nullptr);
    }
    if (fence_) {
        vkDestroyFence(ctx_.device, fence_, nullptr);
    }
    ctx_.Destroy(slots_);
}

uint32_t GpuBatch::Dispatch(Path p, GpuView src, GpuView dst, uint32_t count, uint32_t seed_in,
                            uint32_t seed_out, uint32_t mode, bool want_slot) {
    if (ended_) {
        throw std::runtime_error("GpuBatch already submitted");
    }
    uint32_t slot = max_slots_ - 1; // scratch slot for write-only ops
    if (want_slot) {
        if (used_slots_ + 1 >= max_slots_) {
            throw std::runtime_error("GpuBatch: out of result slots");
        }
        slot = used_slots_++;
    }
    const uint32_t groups = std::clamp<uint32_t>((count + 63) / 64, 1, 4096);
    if (p == Path::Bda) {
        if (!ctx_.bda_pipe) {
            throw Unsupported("BDA path needs bufferDeviceAddress and shaderInt64");
        }
        BdaPush pc{src.addr, dst.addr, slots_.addr + slot * slot_stride_, count, seed_in,
                   seed_out, mode};
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, ctx_.bda_pipe);
        vkCmdPushConstants(cmd_, ctx_.bda_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    } else {
        if ((src.buf && src.offset % ctx_.ssbo_align) ||
            (dst.buf && dst.offset % ctx_.ssbo_align)) {
            throw std::runtime_error(
                "SSBO view offset is not minStorageBufferOffsetAlignment aligned");
        }
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool_;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &ctx_.ssbo_dsl;
        VkDescriptorSet set;
        E0B_CHECK(vkAllocateDescriptorSets(ctx_.device, &dai, &set));
        const GpuView s = src.buf ? src : dst;
        const GpuView d = dst.buf ? dst : src;
        const VkDeviceSize range = std::max<VkDeviceSize>(4, VkDeviceSize(count) * 4);
        VkDescriptorBufferInfo infos[3] = {
            {s.buf, s.offset, range},
            {d.buf, d.offset, range},
            {slots_.buf, slot * slot_stride_, 16},
        };
        VkWriteDescriptorSet w[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(ctx_.device, 3, w, 0, nullptr);
        SsboPush pc{count, seed_in, seed_out, mode};
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, ctx_.ssbo_pipe);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_COMPUTE, ctx_.ssbo_layout, 0, 1, &set,
                                0, nullptr);
        vkCmdPushConstants(cmd_, ctx_.ssbo_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    }
    vkCmdDispatch(cmd_, groups, 1, 1);
    return slot;
}

uint32_t GpuBatch::Verify(Path p, GpuView src, uint32_t count, uint32_t seed) {
    return Dispatch(p, src, {}, count, seed, 0, MODE_VERIFY, true);
}

uint32_t GpuBatch::NonZero(Path p, GpuView src, uint32_t count) {
    return Dispatch(p, src, {}, count, 0, 0, MODE_NONZERO, true);
}

void GpuBatch::Write(Path p, GpuView dst, uint32_t count, uint32_t seed) {
    Dispatch(p, {}, dst, count, 0, seed, MODE_WRITE, false);
}

void GpuBatch::Barrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void GpuBatch::HostBarrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         1, &mb, 0, nullptr, 0, nullptr);
}

void GpuBatch::Submit(std::optional<uint64_t> wait_value, std::optional<uint64_t> signal_value,
                      bool host_wait) {
    if (!ended_) {
        E0B_CHECK(vkEndCommandBuffer(cmd_));
        ended_ = true;
    }
    const uint64_t wv = wait_value.value_or(0);
    const uint64_t sv = signal_value.value_or(0);
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo tl{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tl.waitSemaphoreValueCount = wait_value ? 1 : 0;
    tl.pWaitSemaphoreValues = &wv;
    tl.signalSemaphoreValueCount = signal_value ? 1 : 0;
    tl.pSignalSemaphoreValues = &sv;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &tl;
    si.waitSemaphoreCount = wait_value ? 1 : 0;
    si.pWaitSemaphores = &ctx_.timeline;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = signal_value ? 1 : 0;
    si.pSignalSemaphores = &ctx_.timeline;
    ctx_.CheckResult(vkResetFences(ctx_.device, 1, &fence_), "vkResetFences");
    ctx_.CheckResult(vkQueueSubmit(ctx_.queue, 1, &si, fence_), "vkQueueSubmit");
    pending_ = true;
    if (host_wait) {
        WaitHost();
    }
}

void GpuBatch::Resubmit() {
    if (pending_) {
        WaitHost();
    }
    Submit({}, {}, true);
}

void GpuBatch::WaitHost() {
    if (!pending_) {
        return;
    }
    ctx_.CheckResult(vkWaitForFences(ctx_.device, 1, &fence_, VK_TRUE, kWaitTimeoutNs),
                     "vkWaitForFences(batch)");
    pending_ = false;
}

SlotResult GpuBatch::Slot(uint32_t idx) const {
    const auto* s = reinterpret_cast<const volatile uint32_t*>(
        static_cast<const uint8_t*>(slots_.map) + idx * slot_stride_);
    return {s[0], s[1], s[2], s[3]};
}

} // namespace e0b
