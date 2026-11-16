// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// UMA GPUVM probe. Answers one architectural question for the shadPS4 UMA backend:
// can imported guest physical memory be bound into a sparse "guest VA" buffer, so that the
// host GPU page tables mirror the guest mapping (aliases, noncontiguous physical pieces and
// remaps handled by binds), instead of serving each request from a per-chunk buffer?
//
// Tests (each prints PASS / FAIL / SKIP with the reason):
//   Q   external buffer properties for {host pointer, dma-buf} x {plain, sparse} buffers
//   S1  sparse arena bound to imported backing in permuted order + one alias; GPU reads
//   S2  GPU write through the arena lands in the memfd at the physical offset
//   S3  remap: rebind one VA block to another physical block, GPU reads the new bytes
//       S1-S3 also run as a control on driver-allocated memory, to separate driver sparse
//       support from imported-memory support.
//   B1  copy bandwidth: imported backing vs device-local memory (information only)
//   E1  GPU-owned backing: export a host-visible allocation as dma-buf and mmap it in
//       16 KiB pieces at permuted offsets into one CPU VA range (inverse ownership)
//
// Build: cc -O1 -g -o uma_gpuvm_probe uma_gpuvm_probe.c -lvulkan
// Run:   ./uma_gpuvm_probe [--device N] [--backend auto|host|dmabuf|control] [--mib N] [--bw-mib N,
// 0 skips] Exit:  0 if every run test passed, 1 if any failed, 2 on setup error.

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#if __has_include(<linux/udmabuf.h>)
#include <linux/udmabuf.h>
#define HAVE_UDMABUF 1
#endif

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        VkResult r_ = (x);                                                                         \
        if (r_ != VK_SUCCESS) {                                                                    \
            fprintf(stderr, "%s:%d %s -> %d\n", __FILE__, __LINE__, #x, r_);                       \
            exit(2);                                                                               \
        }                                                                                          \
    } while (0)

static VkInstance inst;
static VkPhysicalDevice pdev;
static VkDevice dev;
static VkPhysicalDeviceMemoryProperties memprops;
static VkPhysicalDeviceProperties props;
static uint32_t work_family = UINT32_MAX, sparse_family = UINT32_MAX;
static VkQueue work_queue, sparse_queue;
static VkCommandPool pool;
static int has_host, has_fd, has_dmabuf;
static int failures, passes;

static void result(const char* id, int ok, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void result(const char* id, int ok, const char* fmt, ...) {
    va_list ap;
    printf("%-3s %s ", id, ok > 0 ? "PASS" : ok == 0 ? "FAIL" : "SKIP");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    if (ok == 0)
        failures++;
    if (ok > 0)
        passes++;
}

static int has_ext(VkExtensionProperties* exts, uint32_t n, const char* name) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(exts[i].extensionName, name))
            return 1;
    return 0;
}

static void setup(int device_index) {
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &app};
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice* devs = calloc(n, sizeof(*devs));
    vkEnumeratePhysicalDevices(inst, &n, devs);
    if (device_index < 0 || (uint32_t)device_index >= n) {
        fprintf(stderr, "no device %d (found %u)\n", device_index, n);
        exit(2);
    }
    pdev = devs[device_index];
    free(devs);

    VkPhysicalDeviceDriverProperties drv = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                      .pNext = &drv};
    vkGetPhysicalDeviceProperties2(pdev, &p2);
    props = p2.properties;
    vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);
    printf("device: %s, driver: %s %s, api %u.%u.%u\n", props.deviceName, drv.driverName,
           drv.driverInfo, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    printf("sparseAddressSpaceSize=%#llx maxMemoryAllocationCount=%u\n",
           (unsigned long long)props.limits.sparseAddressSpaceSize,
           props.limits.maxMemoryAllocationCount);
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags;
        printf("memtype %2u heap %u:%s%s%s%s\n", i, memprops.memoryTypes[i].heapIndex,
               f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? " DEVICE_LOCAL" : "",
               f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? " HOST_VISIBLE" : "",
               f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? " COHERENT" : "",
               f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? " CACHED" : "");
    }

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qn, NULL);
    VkQueueFamilyProperties* qf = calloc(qn, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(pdev, &qn, qf);
    for (uint32_t i = 0; i < qn; i++) {
        if (work_family == UINT32_MAX && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
            work_family = i;
    }
    if (qf[work_family].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)
        sparse_family = work_family;
    for (uint32_t i = 0; i < qn && sparse_family == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT)
            sparse_family = i;
    printf("queue families: work=%u sparse=%d\n", work_family,
           sparse_family == UINT32_MAX ? -1 : (int)sparse_family);
    free(qf);

    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pdev, NULL, &en, NULL);
    VkExtensionProperties* exts = calloc(en, sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(pdev, NULL, &en, exts);
    has_host = has_ext(exts, en, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    has_fd = has_ext(exts, en, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    has_dmabuf = has_fd && has_ext(exts, en, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    free(exts);
    const char* names[3];
    uint32_t nn = 0;
    if (has_host)
        names[nn++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    if (has_fd)
        names[nn++] = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
    if (has_dmabuf)
        names[nn++] = VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME;
    printf("extensions: external_memory_host=%d external_memory_fd=%d dma_buf=%d\n", has_host,
           has_fd, has_dmabuf);

    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(pdev, &feats);
    printf("features: sparseBinding=%u sparseResidencyBuffer=%u sparseResidencyAliased=%u\n",
           feats.sparseBinding, feats.sparseResidencyBuffer, feats.sparseResidencyAliased);
    VkPhysicalDeviceFeatures enable = {.sparseBinding = feats.sparseBinding,
                                       .sparseResidencyBuffer = feats.sparseResidencyBuffer,
                                       .sparseResidencyAliased = feats.sparseResidencyAliased};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci[2] = {
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = work_family,
         .queueCount = 1,
         .pQueuePriorities = &prio},
        {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
         .queueFamilyIndex = sparse_family,
         .queueCount = 1,
         .pQueuePriorities = &prio},
    };
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount =
                                  (sparse_family == UINT32_MAX || sparse_family == work_family) ? 1
                                                                                                : 2,
                              .pQueueCreateInfos = qci,
                              .enabledExtensionCount = nn,
                              .ppEnabledExtensionNames = names,
                              .pEnabledFeatures = &enable};
    CHECK(vkCreateDevice(pdev, &dci, NULL, &dev));
    vkGetDeviceQueue(dev, work_family, 0, &work_queue);
    if (sparse_family != UINT32_MAX)
        vkGetDeviceQueue(dev, sparse_family, 0, &sparse_queue);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = work_family};
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
}

static const char* features_str(VkExternalMemoryFeatureFlags f) {
    static char buf[64];
    snprintf(buf, sizeof buf, "%s%s%s",
             f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ? "importable " : "",
             f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ? "exportable " : "",
             f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ? "dedicated-only" : "");
    return buf[0] ? buf : "none";
}

static VkExternalMemoryFeatureFlags query_external(VkExternalMemoryHandleTypeFlagBits type,
                                                   VkBufferCreateFlags flags) {
    VkPhysicalDeviceExternalBufferInfo info = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
        .flags = flags,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .handleType = type};
    VkExternalBufferProperties out = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vkGetPhysicalDeviceExternalBufferProperties(pdev, &info, &out);
    return out.externalMemoryProperties.externalMemoryFeatures;
}

static uint32_t find_type(uint32_t bits, VkMemoryPropertyFlags need) {
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & need) == need)
            return i;
    return UINT32_MAX;
}

static VkCommandBuffer begin(void) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT |
                                           VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb,
                         0, NULL, 0, NULL);
}

static void submit_wait(VkCommandBuffer cb) {
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(work_queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(work_queue));
    vkFreeCommandBuffers(dev, pool, 1, &cb);
}

// Host-visible readback buffer.
typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t* map;
} Readback;

static Readback make_readback(VkDeviceSize size) {
    Readback r = {0};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    CHECK(vkCreateBuffer(dev, &bci, NULL, &r.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, r.buf, &req);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, &r.mem));
    CHECK(vkBindBufferMemory(dev, r.buf, r.mem, 0));
    CHECK(vkMapMemory(dev, r.mem, 0, VK_WHOLE_SIZE, 0, (void**)&r.map));
    return r;
}

static void free_readback(Readback* r) {
    vkDestroyBuffer(dev, r->buf, NULL);
    vkFreeMemory(dev, r->mem, NULL);
}

// Imports [base, base+size) of the memfd/mapping. Returns VK_NULL_HANDLE and a reason on failure.
static VkDeviceMemory import_backing(int use_dmabuf, int memfd, uint8_t* base, VkDeviceSize size,
                                     uint32_t allowed_bits, const char** why) {
    static char reason[256];
    uint32_t ext_bits = 0;
    int fd = -1;
    if (use_dmabuf) {
#ifdef HAVE_UDMABUF
        int ctl = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        if (ctl < 0) {
            snprintf(reason, sizeof reason, "open /dev/udmabuf: %s", strerror(errno));
            *why = reason;
            return VK_NULL_HANDLE;
        }
        struct udmabuf_create c = {
            .memfd = (uint32_t)memfd, .flags = UDMABUF_FLAGS_CLOEXEC, .offset = 0, .size = size};
        fd = ioctl(ctl, UDMABUF_CREATE, &c);
        close(ctl);
        if (fd < 0) {
            snprintf(reason, sizeof reason, "UDMABUF_CREATE: %s", strerror(errno));
            *why = reason;
            return VK_NULL_HANDLE;
        }
        PFN_vkGetMemoryFdPropertiesKHR getfd =
            (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
        VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (getfd(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp) != VK_SUCCESS) {
            close(fd);
            *why = "vkGetMemoryFdPropertiesKHR failed";
            return VK_NULL_HANDLE;
        }
        ext_bits = fp.memoryTypeBits;
#else
        *why = "built without linux/udmabuf.h";
        return VK_NULL_HANDLE;
#endif
    } else {
        PFN_vkGetMemoryHostPointerPropertiesEXT gethp =
            (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(
                dev, "vkGetMemoryHostPointerPropertiesEXT");
        VkMemoryHostPointerPropertiesEXT hp = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        VkResult r = gethp(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &hp);
        if (r != VK_SUCCESS) {
            snprintf(reason, sizeof reason, "vkGetMemoryHostPointerPropertiesEXT -> %d", r);
            *why = reason;
            return VK_NULL_HANDLE;
        }
        ext_bits = hp.memoryTypeBits;
    }
    uint32_t type = find_type(ext_bits & allowed_bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        snprintf(reason, sizeof reason, "no coherent type in external=%#x & resource=%#x", ext_bits,
                 allowed_bits);
        if (fd >= 0)
            close(fd);
        *why = reason;
        return VK_NULL_HANDLE;
    }
    VkImportMemoryFdInfoKHR fdi = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   .fd = fd};
    VkImportMemoryHostPointerInfoEXT hpi = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        .pHostPointer = base};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = use_dmabuf ? (void*)&fdi : (void*)&hpi,
                               .allocationSize = size,
                               .memoryTypeIndex = type};
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) {
        if (fd >= 0)
            close(fd);
        snprintf(reason, sizeof reason, "vkAllocateMemory(import, type %u) -> %d", type, r);
        *why = reason;
        return VK_NULL_HANDLE;
    }
    printf("    imported %s backing: %llu MiB, memory type %u (external bits %#x)\n",
           use_dmabuf ? "dma-buf" : "host-pointer", (unsigned long long)(size >> 20), type,
           ext_bits);
    return mem;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int bind_blocks(VkBuffer arena, VkDeviceMemory mem, const uint32_t* va_to_phys,
                       uint32_t count, VkDeviceSize block, const char** why) {
    VkSparseMemoryBind* binds = calloc(count, sizeof(*binds));
    for (uint32_t i = 0; i < count; i++) {
        binds[i].resourceOffset = (VkDeviceSize)i * block;
        binds[i].size = block;
        binds[i].memory = va_to_phys[i] == UINT32_MAX ? VK_NULL_HANDLE : mem;
        binds[i].memoryOffset =
            va_to_phys[i] == UINT32_MAX ? 0 : (VkDeviceSize)va_to_phys[i] * block;
    }
    VkSparseBufferMemoryBindInfo bi = {.buffer = arena, .bindCount = count, .pBinds = binds};
    VkBindSparseInfo info = {
        .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .bufferBindCount = 1, .pBufferBinds = &bi};
    const double start = now_ms();
    VkResult r = vkQueueBindSparse(sparse_queue, 1, &info, VK_NULL_HANDLE);
    free(binds);
    if (r != VK_SUCCESS) {
        static char reason[64];
        snprintf(reason, sizeof reason, "vkQueueBindSparse -> %d", r);
        *why = reason;
        return 0;
    }
    // The probe keeps it simple: binds and work never overlap.
    CHECK(vkQueueWaitIdle(sparse_queue));
    printf("    bind: %u ops of %llu KiB in %.3f ms (submit + idle)\n", count,
           (unsigned long long)(block >> 10), now_ms() - start);
    return 1;
}

// mode: 0 host pointer, 1 dma-buf, 2 control (driver-allocated memory, no import)
static void sparse_tests(int mode, VkDeviceSize total) {
    const int use_dmabuf = mode == 1, control = mode == 2;
    const VkExternalMemoryHandleTypeFlagBits type =
        use_dmabuf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                   : VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    const char* name = control ? "control" : use_dmabuf ? "dma-buf" : "host-pointer";
    if (sparse_family == UINT32_MAX) {
        result("S1", -1, "%s: no sparse binding queue", name);
        return;
    }
    VkExternalMemoryFeatureFlags f = query_external(
        type, VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT);
    if (!control && !(f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
        printf("    note: driver reports %s not importable for sparse buffers; trying anyway\n",
               name);

    // Guest physical backing: a sealable memfd mapped MAP_SHARED, as AddressSpace does.
    int memfd = memfd_create("uma-probe", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (memfd < 0 || ftruncate(memfd, total) < 0) {
        result("S1", -1, "%s: memfd: %s", name, strerror(errno));
        return;
    }
    fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK);
    uint8_t* phys = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    if (phys == MAP_FAILED) {
        result("S1", -1, "%s: mmap: %s", name, strerror(errno));
        close(memfd);
        return;
    }

    // Sparse "guest VA" arena with the external handle type, as
    // VUID-VkSparseMemoryBind-memory-02731 requires.
    VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = type};
    uint32_t families[2] = {work_family, sparse_family};
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = control ? NULL : &ext,
        .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT |
                 VK_BUFFER_CREATE_SPARSE_ALIASED_BIT,
        .size = total,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode =
            sparse_family == work_family ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = sparse_family == work_family ? 0 : 2,
        .pQueueFamilyIndices = families};
    VkBuffer arena;
    VkResult cr = vkCreateBuffer(dev, &bci, NULL, &arena);
    if (cr != VK_SUCCESS) {
        result("S1", 0, "%s: vkCreateBuffer(sparse, external) -> %d", name, cr);
        munmap(phys, total);
        close(memfd);
        return;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, arena, &req);
    const VkDeviceSize block = req.alignment;
    const uint32_t count = (uint32_t)(total / block);
    printf("    %s sparse arena: block=%llu KiB, blocks=%u, memoryTypeBits=%#x\n", name,
           (unsigned long long)(block >> 10), count, req.memoryTypeBits);

    const char* why = NULL;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (control) {
        // Driver-owned memory, written by the CPU through vkMapMemory instead of the memfd.
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = total,
            .memoryTypeIndex =
                find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        if (ai.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(dev, &ai, NULL, &mem) != VK_SUCCESS) {
            result("S1", -1, "control: no host-visible sparse-compatible memory");
            vkDestroyBuffer(dev, arena, NULL);
            munmap(phys, total);
            close(memfd);
            return;
        }
        munmap(phys, total);
        CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void**)&phys));
    } else
        mem = import_backing(use_dmabuf, memfd, phys, total, req.memoryTypeBits, &why);
    if (!mem) {
        result("S1", 0, "%s: import into a sparse-compatible type failed: %s", name, why);
        vkDestroyBuffer(dev, arena, NULL);
        munmap(phys, total);
        close(memfd);
        return;
    }

    // Physical block p holds words (p << 20 | word index).
    const uint32_t words = (uint32_t)(block / 4);
    for (uint32_t p = 0; p < count; p++)
        for (uint32_t w = 0; w < words; w++)
            ((uint32_t*)phys)[(size_t)p * words + w] = (p << 20) | (w & 0xfffff);

    // VA block v -> physical block (count - 1 - v): every neighbour pair is physically
    // noncontiguous. The last VA block aliases physical block 0 a second time.
    uint32_t* map = calloc(count, sizeof(*map));
    for (uint32_t v = 0; v < count; v++)
        map[v] = count - 1 - v;
    if (count > 2)
        map[count - 1] = map[0];
    if (!bind_blocks(arena, mem, map, count, block, &why)) {
        result("S1", 0, "%s: %s", name, why);
        goto out;
    }

    Readback rb = make_readback(total);
    VkCommandBuffer cb = begin();
    VkBufferCopy copy = {.srcOffset = 0, .dstOffset = 0, .size = total};
    vkCmdCopyBuffer(cb, arena, rb.buf, 1, &copy);
    barrier_all(cb);
    submit_wait(cb);
    uint32_t bad = 0, first_bad = UINT32_MAX;
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t w = 0; w < words; w += 997)
            if (rb.map[(size_t)v * words + w] != ((map[v] << 20) | (w & 0xfffff))) {
                bad++;
                if (first_bad == UINT32_MAX)
                    first_bad = v;
            }
    result("S1", bad == 0,
           "%s: GPU reads through a sparse VA arena bound to %s backing, %u noncontiguous "
           "blocks + 1 alias (%u mismatches, first VA block %d)",
           name, control ? "driver-allocated" : "imported", count, bad,
           first_bad == UINT32_MAX ? -1 : (int)first_bad);

    // S2: GPU write through VA block 1 lands at its physical block in the memfd mapping.
    const uint32_t target = 1;
    cb = begin();
    vkCmdFillBuffer(cb, arena, (VkDeviceSize)target * block, block, 0xC0FFEE00u);
    barrier_all(cb);
    submit_wait(cb);
    uint32_t* pw = (uint32_t*)(phys + (size_t)map[target] * block);
    bad = 0;
    for (uint32_t w = 0; w < words; w++)
        bad += pw[w] != 0xC0FFEE00u;
    result("S2", bad == 0,
           "%s: GPU fill through VA block %u visible at physical block %u (%u mismatches)", name,
           target, map[target], bad);

    // S3: remap VA block 0 from physical block map[0] to physical block 2, then read it again.
    const uint32_t old_phys = map[0];
    map[0] = 2 % count;
    if (!bind_blocks(arena, mem, map, 1, block, &why)) {
        result("S3", 0, "%s: rebind: %s", name, why);
    } else {
        cb = begin();
        VkBufferCopy c0 = {.srcOffset = 0, .dstOffset = 0, .size = block};
        vkCmdCopyBuffer(cb, arena, rb.buf, 1, &c0);
        barrier_all(cb);
        submit_wait(cb);
        bad = 0;
        for (uint32_t w = 0; w < words; w += 61)
            bad += rb.map[w] != ((map[0] << 20) | (w & 0xfffff));
        result("S3", bad == 0,
               "%s: remap VA block 0 phys %u -> %u by rebind, GPU sees new bytes (%u mismatches)",
               name, old_phys, map[0], bad);
    }
    free_readback(&rb);
out:
    free(map);
    vkDestroyBuffer(dev, arena, NULL);
    if (control)
        vkUnmapMemory(dev, mem);
    vkFreeMemory(dev, mem, NULL);
    if (!control)
        munmap(phys, total);
    close(memfd);
}

// E1: inverse ownership. The driver allocates host-visible memory exportable as dma-buf; the CPU
// maps that fd in 16 KiB pieces at permuted offsets into one reserved VA range, as AddressSpace
// would map guest pages; the GPU writes through a buffer bound to the allocation.
static void exported_backing_test(VkDeviceSize total) {
    if (!has_dmabuf) {
        result("E1", -1, "VK_EXT_external_memory_dma_buf unavailable");
        return;
    }
    VkExternalMemoryFeatureFlags f =
        query_external(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, 0);
    if (!(f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        result("E1", -1, "dma-buf buffers not exportable (%s)", features_str(f));
        return;
    }
    VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .pNext = &ext,
                              .size = total,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, buf, &req);
    // Prefer cached coherent (Onion-like); fall back to any coherent host-visible type.
    uint32_t type = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX)
        type = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                         .buffer = buf};
    VkExportMemoryAllocateInfo exp = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ? &ded : NULL,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &exp,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
    VkDeviceMemory mem;
    VkResult r = vkAllocateMemory(dev, &ai, NULL, &mem);
    if (r != VK_SUCCESS) {
        result("E1", 0, "exportable allocation (type %u) -> %d", type, r);
        vkDestroyBuffer(dev, buf, NULL);
        return;
    }
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    PFN_vkGetMemoryFdKHR getfd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdKHR");
    VkMemoryGetFdInfoKHR gi = {.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                               .memory = mem,
                               .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    int fd = -1;
    r = getfd(dev, &gi, &fd);
    if (r != VK_SUCCESS) {
        result("E1", 0, "vkGetMemoryFdKHR -> %d", r);
        goto out;
    }
    const size_t piece = 16 << 10;
    const size_t pieces = total / piece;
    uint8_t* va = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    int mapped_ok = va != MAP_FAILED;
    // CPU VA piece i -> dma-buf offset (pieces - 1 - i) * 16 KiB.
    for (size_t i = 0; mapped_ok && i < pieces; i++) {
        void* p = mmap(va + i * piece, piece, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
                       (off_t)((pieces - 1 - i) * piece));
        if (p == MAP_FAILED) {
            result("E1", 0, "mmap dma-buf piece %zu at offset %#zx: %s (cached type %u)", i,
                   (pieces - 1 - i) * piece, strerror(errno), type);
            mapped_ok = 0;
        }
    }
    if (mapped_ok) {
        Readback rb = make_readback(total);
        for (size_t i = 0; i < pieces; i++)
            ((uint32_t*)(va + i * piece))[3] = 0xAB000000u | (uint32_t)i;
        VkCommandBuffer cb = begin();
        VkBufferCopy copy = {.size = total};
        vkCmdCopyBuffer(cb, buf, rb.buf, 1, &copy);
        barrier_all(cb);
        submit_wait(cb);
        uint32_t bad = 0;
        for (size_t i = 0; i < pieces; i++)
            bad += rb.map[((pieces - 1 - i) * piece) / 4 + 3] != (0xAB000000u | (uint32_t)i);
        result("E1", bad == 0,
               "driver-owned dma-buf backing (memory type %u%s) mapped in %zu permuted 16 KiB "
               "CPU pieces; CPU writes seen by GPU (%u mismatches)",
               type,
               (memprops.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                   ? ", cached"
                   : "",
               pieces, bad);
        free_readback(&rb);
    }
    if (va != MAP_FAILED)
        munmap(va, total);
    close(fd);
out:
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
}

// B1: copy bandwidth between imported guest backing and driver-owned device-local memory.
// Input for placement: on an APU both are system RAM; on a discrete GPU imported memory is
// reached over PCIe.
static VkBuffer make_buffer(VkDeviceSize size, void* pnext) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .pNext = pnext,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &b));
    return b;
}

static double time_copy(VkBuffer src, VkBuffer dst, VkDeviceSize size, int reps) {
    VkCommandBuffer cb = begin();
    VkBufferCopy c = {.size = size};
    vkCmdCopyBuffer(cb, src, dst, 1, &c); // warm-up
    barrier_all(cb);
    submit_wait(cb);
    cb = begin();
    for (int i = 0; i < reps; i++) {
        vkCmdCopyBuffer(cb, src, dst, 1, &c);
        barrier_all(cb);
    }
    const double start = now_ms();
    submit_wait(cb);
    const double ms = now_ms() - start;
    return (double)size * reps / (ms / 1e3) / 1e9;
}

static void bandwidth_test(int use_dmabuf, VkDeviceSize size) {
    const char* name = use_dmabuf ? "dma-buf" : "host-pointer";
    const VkExternalMemoryHandleTypeFlagBits type =
        use_dmabuf ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                   : VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    int memfd = memfd_create("uma-probe-bw", MFD_ALLOW_SEALING | MFD_CLOEXEC);
    if (memfd < 0 || ftruncate(memfd, size) < 0) {
        printf("B1  SKIP %s: memfd: %s\n", name, strerror(errno));
        return;
    }
    fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK);
    uint8_t* phys = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, 0);
    memset(phys, 0x5a, size);
    VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = type};
    VkBuffer shared = make_buffer(size, &ext);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, shared, &req);
    const char* why = NULL;
    VkDeviceMemory smem = import_backing(use_dmabuf, memfd, phys, size, req.memoryTypeBits, &why);
    if (!smem) {
        printf("B1  SKIP %s: %s\n", name, why);
        vkDestroyBuffer(dev, shared, NULL);
        munmap(phys, size);
        close(memfd);
        return;
    }
    CHECK(vkBindBufferMemory(dev, shared, smem, 0));
    VkBuffer local = make_buffer(size, NULL);
    vkGetBufferMemoryRequirements(dev, local, &req);
    uint32_t ltype = UINT32_MAX;
    for (uint32_t i = 0; i < memprops.memoryTypeCount && ltype == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
            !(memprops.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            ltype = i;
    if (ltype == UINT32_MAX)
        ltype = find_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = ltype};
    VkDeviceMemory lmem, lmem2;
    CHECK(vkAllocateMemory(dev, &ai, NULL, &lmem));
    CHECK(vkBindBufferMemory(dev, local, lmem, 0));
    VkBuffer local2 = make_buffer(size, NULL);
    CHECK(vkAllocateMemory(dev, &ai, NULL, &lmem2));
    CHECK(vkBindBufferMemory(dev, local2, lmem2, 0));
    const int reps = 8;
    printf("B1  INFO %s, %llu MiB x %d: local->local %.1f GB/s, shared->local %.1f GB/s, "
           "local->shared %.1f GB/s (device-local type %u)\n",
           name, (unsigned long long)(size >> 20), reps, time_copy(local, local2, size, reps),
           time_copy(shared, local, size, reps), time_copy(local, shared, size, reps), ltype);
    vkDestroyBuffer(dev, local, NULL);
    vkDestroyBuffer(dev, local2, NULL);
    vkDestroyBuffer(dev, shared, NULL);
    vkFreeMemory(dev, lmem, NULL);
    vkFreeMemory(dev, lmem2, NULL);
    vkFreeMemory(dev, smem, NULL);
    munmap(phys, size);
    close(memfd);
}

int main(int argc, char** argv) {
    int device_index = 0;
    const char* backend = "auto";
    VkDeviceSize mib = 8, bw_mib = 64;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--device"))
            device_index = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--backend"))
            backend = argv[i + 1];
        else if (!strcmp(argv[i], "--mib"))
            mib = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--bw-mib"))
            bw_mib = strtoull(argv[i + 1], NULL, 10);
    }
    setup(device_index);

    const VkExternalMemoryHandleTypeFlagBits types[2] = {
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    const char* tnames[2] = {"host-pointer", "dma-buf"};
    for (int t = 0; t < 2; t++) {
        if ((t == 0 && !has_host) || (t == 1 && !has_dmabuf))
            continue;
        printf("Q   %-12s plain buffer: %s\n", tnames[t],
               features_str(query_external(types[t], 0)));
        printf("Q   %-12s sparse buffer: %s\n", tnames[t],
               features_str(query_external(types[t], VK_BUFFER_CREATE_SPARSE_BINDING_BIT |
                                                         VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT)));
    }

    const VkDeviceSize total = mib << 20;
    int want_dmabuf = !strcmp(backend, "dmabuf") || (!strcmp(backend, "auto") && has_dmabuf &&
                                                     access("/dev/udmabuf", R_OK | W_OK) == 0);
    int want_host = !strcmp(backend, "host") || (!strcmp(backend, "auto") && has_host);
    if (!strcmp(backend, "auto") || !strcmp(backend, "control"))
        sparse_tests(2, total);
    if (want_dmabuf)
        sparse_tests(1, total);
    if (want_host)
        sparse_tests(0, total);
    if (!want_dmabuf && !want_host)
        result("S1", -1, "no import backend available");
    exported_backing_test(total);
    if (bw_mib) {
        if (want_dmabuf)
            bandwidth_test(1, bw_mib << 20);
        if (want_host)
            bandwidth_test(0, bw_mib << 20);
    }

    printf("summary: %d passed, %d failed\n", passes, failures);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    return failures ? 1 : 0;
}
