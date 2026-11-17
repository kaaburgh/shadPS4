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
//   T1-T6  shifted imports: the same guest pages imported at every 16 KiB phase of a larger
//       sparse block, bound into one arena; reads and writes agree between all of them
//   U1-U7  stitched imports: one memory object over a list of non-adjacent guest pages
//       (UDMABUF_CREATE_LIST), bound as whole blocks next to ordinary imports of those pages
//   V1-V2  calls that break a rule on purpose, to show under the validation layer that the
//       layer is loaded and checks sparse binds (--backend validation-canary)
//
// Build: cc -O1 -g -o uma_gpuvm_probe uma_gpuvm_probe.c -lvulkan
// Run:   ./uma_gpuvm_probe [--device N] [--backend auto|host|dmabuf|control] [--mib N]
//                          [--bw-mib N, 0 skips]
//        ./uma_gpuvm_probe --backend shifted-dmabuf|shifted-control|stitched-dmabuf|
//                          stitched-control [--block-kib N, default 64]
//                          [--store-mib N, default 2: guest memory covered by the imports]
//        ./uma_gpuvm_probe --backend validation-canary
// Exit:  0 if every run test passed, 1 if any failed, 2 on setup error.

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
static char driver_name[VK_MAX_DRIVER_NAME_SIZE];

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
    // Drivers skip structures of extensions they do not support; the value then stays 0.
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT host_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT,
        .pNext = &drv};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                      .pNext = &host_props};
    vkGetPhysicalDeviceProperties2(pdev, &p2);
    props = p2.properties;
    snprintf(driver_name, sizeof driver_name, "%s", drv.driverName);
    vkGetPhysicalDeviceMemoryProperties(pdev, &memprops);
    printf("device: %s, driver: %s %s, api %u.%u.%u\n", props.deviceName, drv.driverName,
           drv.driverInfo, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    printf("sparseAddressSpaceSize=%#llx maxMemoryAllocationCount=%u\n",
           (unsigned long long)props.limits.sparseAddressSpaceSize,
           props.limits.maxMemoryAllocationCount);
    printf("residencyNonResidentStrict=%u minImportedHostPointerAlignment=%#llx\n",
           props.sparseProperties.residencyNonResidentStrict,
           (unsigned long long)host_props.minImportedHostPointerAlignment);
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

// ---------------------------------------------------------------------------------------------
// T (shifted imports) and U (stitched imports): 16 KiB guest pages against a larger sparse block.
//
// A bind covers whole blocks, and its memory offset is a block multiple. A guest mapping whose
// physical address is not a block multiple away from its virtual address ("phase"), or whose
// block holds pages of several physical runs, therefore needs a memory object that starts at
// another guest page (shifted) or that is built from a page list (stitched). These tests check
// that such objects, bound into one sparse arena next to ordinary imports of the same pages,
// all show the same bytes. See docs/research/uma-gpuvm-granularity.md, CE1 to CE4.
//
// Every check compares the whole arena as the GPU reads it, and the whole of guest memory as
// the CPU reads it, with a model that the probe keeps on the side. A write that lands on a
// wrong page, or that one view does not see, is a mismatch.
//
// Each test runs in two modes:
//   dma-buf  imports of udmabuf objects over one memfd, binds in multiples of the test block
//   control  one driver allocation, the same layout expressed with 16 KiB binds. It checks the
//            test itself and is possible only where the driver binds at 16 KiB or finer.
#define GUEST_PAGE ((VkDeviceSize)16 << 10)
#define PAGE_WORDS ((uint32_t)(GUEST_PAGE / 4))
#define MAX_BINDS 96
#define MAX_IMPORTS 32
#define MAX_PHASES 16

typedef struct {
    int memfd;
    uint32_t* cpu;      // CPU view of the pages
    VkDeviceMemory mem; // control mode: the allocation that holds them
} Store;

typedef struct {
    uint32_t store, page;
} Piece;

typedef struct {
    const char* name;
    int control;
    VkDeviceSize block, align, arena_size, store_size;
    uint32_t r;          // guest pages per block
    uint32_t phys_pages; // guest pages per store
    Store store[2];      // 0: guest physical memory. 1: a private copy with the same bytes.
    uint32_t* model;     // expected words, page id = store * phys_pages + page
    VkBuffer arena;
    VkMemoryRequirements req;
    uint32_t arena_pages;
    uint32_t* table; // arena page -> page id
    VkDeviceMemory phase[2][MAX_PHASES];
    VkDeviceMemory imports[MAX_IMPORTS];
    uint32_t import_count;
    VkSparseMemoryBind binds[MAX_BINDS];
    uint32_t bind_count;
    Readback rb;
    double create_ms, import_ms, bind_ms; // of the last object and bind
    uint32_t last_items;
    char detail[160]; // first mismatch
} World;

static uint32_t pattern(uint32_t id, uint32_t word) {
    return 0x40000000u | (id << 12) | word;
}

static long long gtt_used(void) {
    long long total = -1;
    for (int card = 0; card < 4; card++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/class/drm/card%d/device/mem_info_gtt_used", card);
        FILE* f = fopen(path, "r");
        long long v;
        if (f && fscanf(f, "%lld", &v) == 1)
            total = (total < 0 ? 0 : total) + v;
        if (f)
            fclose(f);
    }
    return total;
}

static void world_free(World* w) {
    if (w->rb.buf)
        free_readback(&w->rb);
    if (w->arena)
        vkDestroyBuffer(dev, w->arena, NULL);
    for (uint32_t i = 0; i < w->import_count; i++)
        vkFreeMemory(dev, w->imports[i], NULL);
    for (int s = 0; s < 2; s++) {
        if (w->control && w->store[s].mem) {
            vkUnmapMemory(dev, w->store[s].mem);
            vkFreeMemory(dev, w->store[s].mem, NULL);
        } else if (w->store[s].cpu) {
            munmap(w->store[s].cpu, w->store_size);
            close(w->store[s].memfd);
        }
    }
    free(w->model);
    free(w->table);
}

// Returns 1 when ready, -1 when the test cannot run here (why says so), 0 on a failure.
static int world_init(World* w, int control, VkDeviceSize block, uint32_t phys_blocks,
                      uint32_t arena_blocks, const char** why) {
    static char reason[200];
    *why = reason;
    memset(w, 0, sizeof *w);
    w->control = control;
    w->name = control ? "control" : "dma-buf";
    w->block = block;
    w->r = (uint32_t)(block / GUEST_PAGE);
    w->phys_pages = phys_blocks * w->r;
    w->store_size = (VkDeviceSize)phys_blocks * block;
    w->arena_size = (VkDeviceSize)arena_blocks * block;
    w->arena_pages = arena_blocks * w->r;
    if (block % GUEST_PAGE || w->r < 2 || w->r > MAX_PHASES) {
        snprintf(reason, sizeof reason,
                 "a block of %llu KiB has no second 16 KiB phase: nothing to test",
                 (unsigned long long)(block >> 10));
        return -1;
    }
    if (sparse_family == UINT32_MAX) {
        snprintf(reason, sizeof reason, "no sparse binding queue");
        return -1;
    }
    if (!control && (!has_dmabuf || access("/dev/udmabuf", R_OK | W_OK) != 0)) {
        snprintf(reason, sizeof reason, "needs VK_EXT_external_memory_dma_buf and /dev/udmabuf");
        return -1;
    }
    VkExternalMemoryBufferCreateInfo ext = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    uint32_t families[2] = {work_family, sparse_family};
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = control ? NULL : &ext,
        .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT |
                 VK_BUFFER_CREATE_SPARSE_ALIASED_BIT,
        .size = w->arena_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode =
            sparse_family == work_family ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = sparse_family == work_family ? 0 : 2,
        .pQueueFamilyIndices = families};
    VkResult cr = vkCreateBuffer(dev, &bci, NULL, &w->arena);
    if (cr != VK_SUCCESS) {
        snprintf(reason, sizeof reason, "vkCreateBuffer(sparse) -> %d", cr);
        return 0;
    }
    vkGetBufferMemoryRequirements(dev, w->arena, &w->req);
    w->align = w->req.alignment;
    printf("    %s arena: test block %llu KiB, driver bind granularity %llu KiB, %u blocks\n",
           w->name, (unsigned long long)(block >> 10), (unsigned long long)(w->align >> 10),
           arena_blocks);
    if (block % w->align) {
        snprintf(reason, sizeof reason,
                 "test block %llu KiB is not a multiple of the driver's %llu KiB",
                 (unsigned long long)(block >> 10), (unsigned long long)(w->align >> 10));
        return -1;
    }
    if (control && GUEST_PAGE % w->align) {
        snprintf(reason, sizeof reason,
                 "the driver binds in units of %llu KiB, so one memory object cannot express "
                 "16 KiB phases",
                 (unsigned long long)(w->align >> 10));
        return -1;
    }
    for (int s = 0; s < 2; s++) {
        Store* st = &w->store[s];
        if (control) {
            VkMemoryAllocateInfo ai = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = w->store_size,
                .memoryTypeIndex =
                    find_type(w->req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
            if (ai.memoryTypeIndex == UINT32_MAX ||
                vkAllocateMemory(dev, &ai, NULL, &st->mem) != VK_SUCCESS) {
                st->mem = VK_NULL_HANDLE;
                snprintf(reason, sizeof reason, "no host-visible sparse-compatible memory");
                return -1;
            }
            CHECK(vkMapMemory(dev, st->mem, 0, VK_WHOLE_SIZE, 0, (void**)&st->cpu));
        } else {
            st->memfd = memfd_create("uma-probe-pages", MFD_ALLOW_SEALING | MFD_CLOEXEC);
            if (st->memfd < 0 || ftruncate(st->memfd, (off_t)w->store_size) < 0) {
                snprintf(reason, sizeof reason, "memfd: %s", strerror(errno));
                return 0;
            }
            fcntl(st->memfd, F_ADD_SEALS, F_SEAL_SHRINK);
            st->cpu = mmap(NULL, w->store_size, PROT_READ | PROT_WRITE, MAP_SHARED, st->memfd, 0);
            if (st->cpu == MAP_FAILED) {
                st->cpu = NULL;
                snprintf(reason, sizeof reason, "mmap: %s", strerror(errno));
                return 0;
            }
        }
    }
    w->model = calloc((size_t)2 * w->phys_pages * PAGE_WORDS, 4);
    w->table = calloc(w->arena_pages, sizeof(uint32_t));
    for (uint32_t i = 0; i < w->arena_pages; i++)
        w->table[i] = UINT32_MAX;
    // The copy starts with the bytes of the original, so only its pages tell them apart.
    for (uint32_t page = 0; page < w->phys_pages; page++)
        for (uint32_t word = 0; word < PAGE_WORDS; word++) {
            const uint32_t value = pattern(page, word);
            const size_t at = (size_t)page * PAGE_WORDS + word;
            w->store[0].cpu[at] = w->store[1].cpu[at] = value;
            w->model[at] = w->model[(size_t)w->phys_pages * PAGE_WORDS + at] = value;
        }
    w->rb = make_readback(w->arena_size);
    return 1;
}

// One memory object over the listed guest pages, in list order. Neighbouring pages of one
// store become one udmabuf item, so a physically contiguous list is an ordinary import.
static VkDeviceMemory import_pages(World* w, const Piece* pieces, uint32_t count,
                                   const char** why) {
    static char reason[200];
    *why = reason;
#ifdef HAVE_UDMABUF
    struct udmabuf_create_list* list =
        calloc(1, sizeof(*list) + count * sizeof(struct udmabuf_create_item));
    list->flags = UDMABUF_FLAGS_CLOEXEC;
    for (uint32_t i = 0; i < count; i++) {
        struct udmabuf_create_item* last = list->count ? &list->list[list->count - 1] : NULL;
        const uint64_t offset = (uint64_t)pieces[i].page * GUEST_PAGE;
        if (last && i && pieces[i].store == pieces[i - 1].store &&
            pieces[i].page == pieces[i - 1].page + 1) {
            last->size += GUEST_PAGE;
        } else {
            list->list[list->count++] =
                (struct udmabuf_create_item){.memfd = (uint32_t)w->store[pieces[i].store].memfd,
                                             .offset = offset,
                                             .size = GUEST_PAGE};
        }
    }
    w->last_items = list->count;
    const VkDeviceSize size = (VkDeviceSize)count * GUEST_PAGE;
    int ctl = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
    if (ctl < 0) {
        snprintf(reason, sizeof reason, "open /dev/udmabuf: %s", strerror(errno));
        free(list);
        return VK_NULL_HANDLE;
    }
    double start = now_ms();
    int fd = ioctl(ctl, UDMABUF_CREATE_LIST, list);
    w->create_ms = now_ms() - start;
    const int create_errno = errno;
    close(ctl);
    if (fd < 0) {
        snprintf(reason, sizeof reason, "UDMABUF_CREATE_LIST of %u items: %s", list->count,
                 strerror(create_errno));
        free(list);
        return VK_NULL_HANDLE;
    }
    free(list);
    PFN_vkGetMemoryFdPropertiesKHR getfd =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkResult r = getfd(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
    uint32_t type = UINT32_MAX;
    if (r == VK_SUCCESS)
        type =
            find_type(fp.memoryTypeBits & w->req.memoryTypeBits,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        snprintf(reason, sizeof reason,
                 "no coherent memory type for the dma-buf (properties -> %d, bits %#x, arena %#x)",
                 r, fp.memoryTypeBits, w->req.memoryTypeBits);
        close(fd);
        return VK_NULL_HANDLE;
    }
    VkImportMemoryFdInfoKHR fdi = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   .fd = fd};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &fdi,
                               .allocationSize = size,
                               .memoryTypeIndex = type};
    VkDeviceMemory mem;
    start = now_ms();
    r = vkAllocateMemory(dev, &ai, NULL, &mem);
    w->import_ms = now_ms() - start;
    if (r != VK_SUCCESS || w->import_count == MAX_IMPORTS) {
        snprintf(reason, sizeof reason, "vkAllocateMemory(import of %u pages, type %u) -> %d",
                 count, type, r);
        close(fd); // a failed import leaves the descriptor with the caller
        return VK_NULL_HANDLE;
    }
    w->imports[w->import_count++] = mem;
    return mem;
#else
    (void)w, (void)pieces, (void)count;
    snprintf(reason, sizeof reason, "built without linux/udmabuf.h");
    return VK_NULL_HANDLE;
#endif
}

// Queues one bind after checking it against the rules of VkSparseMemoryBind for buffers:
// offsets and size in units of the driver's granularity, and inside both objects.
static void add_bind(World* w, uint32_t arena_page, uint32_t pages, VkDeviceMemory mem,
                     VkDeviceSize memory_offset, VkDeviceSize memory_size) {
    const VkDeviceSize offset = (VkDeviceSize)arena_page * GUEST_PAGE;
    const VkDeviceSize size = (VkDeviceSize)pages * GUEST_PAGE;
    if (offset % w->align || size % w->align || memory_offset % w->align ||
        offset + size > w->arena_size || memory_offset + size > memory_size ||
        w->bind_count == MAX_BINDS) {
        fprintf(stderr,
                "probe bug: illegal bind offset=%#llx size=%#llx memoryOffset=%#llx "
                "memorySize=%#llx granularity=%#llx\n",
                (unsigned long long)offset, (unsigned long long)size,
                (unsigned long long)memory_offset, (unsigned long long)memory_size,
                (unsigned long long)w->align);
        exit(2);
    }
    w->binds[w->bind_count++] = (VkSparseMemoryBind){
        .resourceOffset = offset, .size = size, .memory = mem, .memoryOffset = memory_offset};
}

static int flush_binds(World* w, const char** why) {
    static char reason[64];
    VkSparseBufferMemoryBindInfo bi = {
        .buffer = w->arena, .bindCount = w->bind_count, .pBinds = w->binds};
    VkBindSparseInfo info = {
        .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .bufferBindCount = 1, .pBufferBinds = &bi};
    const double start = now_ms();
    VkResult r = vkQueueBindSparse(sparse_queue, 1, &info, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) {
        snprintf(reason, sizeof reason, "vkQueueBindSparse -> %d", r);
        *why = reason;
        return 0;
    }
    CHECK(vkQueueWaitIdle(sparse_queue));
    w->bind_ms = now_ms() - start;
    w->bind_count = 0;
    return 1;
}

// Maps whole arena blocks to a physically contiguous run of guest pages that may start at any
// page: the case of a mapping with a phase (CE1) and of an alias with another phase (CE3).
// dma-buf mode binds an import that starts at page (first_page mod r) of the store, so that
// the memory offset of the bind is a block multiple.
static int map_run(World* w, uint32_t arena_block, uint32_t blocks, uint32_t store,
                   uint32_t first_page, const char** why) {
    const uint32_t phase = first_page % w->r, pages = blocks * w->r;
    if (w->control) {
        add_bind(w, arena_block * w->r, pages, w->store[store].mem,
                 (VkDeviceSize)first_page * GUEST_PAGE, w->store_size);
    } else {
        const uint32_t import_pages_count = w->phys_pages - w->r;
        if (!w->phase[store][phase]) {
            Piece* all = calloc(import_pages_count, sizeof(Piece));
            for (uint32_t i = 0; i < import_pages_count; i++)
                all[i] = (Piece){store, phase + i};
            w->phase[store][phase] = import_pages(w, all, import_pages_count, why);
            free(all);
            if (!w->phase[store][phase])
                return 0;
            printf("    import: store %u from page %u (phase %u KiB), %u pages, udmabuf %.3f ms, "
                   "vkAllocateMemory %.3f ms\n",
                   store, phase, phase * 16, import_pages_count, w->create_ms, w->import_ms);
        }
        add_bind(w, arena_block * w->r, pages, w->phase[store][phase],
                 (VkDeviceSize)(first_page - phase) * GUEST_PAGE,
                 (VkDeviceSize)import_pages_count * GUEST_PAGE);
    }
    for (uint32_t i = 0; i < pages; i++)
        w->table[arena_block * w->r + i] = store * w->phys_pages + first_page + i;
    return 1;
}

// Maps whole arena blocks to an arbitrary list of guest pages (CE2, CE4): one stitched
// memory object in dma-buf mode, one bind per page in control mode.
static int map_stitched(World* w, uint32_t arena_block, const Piece* pieces, uint32_t blocks,
                        const char** why) {
    const uint32_t pages = blocks * w->r;
    if (w->control) {
        for (uint32_t i = 0; i < pages; i++)
            add_bind(w, arena_block * w->r + i, 1, w->store[pieces[i].store].mem,
                     (VkDeviceSize)pieces[i].page * GUEST_PAGE, w->store_size);
    } else {
        VkDeviceMemory mem = import_pages(w, pieces, pages, why);
        if (!mem)
            return 0;
        printf("    import: stitched object of %u pages in %u udmabuf items, udmabuf %.3f ms, "
               "vkAllocateMemory %.3f ms\n",
               pages, w->last_items, w->create_ms, w->import_ms);
        add_bind(w, arena_block * w->r, pages, mem, 0, (VkDeviceSize)pages * GUEST_PAGE);
    }
    for (uint32_t i = 0; i < pages; i++)
        w->table[arena_block * w->r + i] = pieces[i].store * w->phys_pages + pieces[i].page;
    return 1;
}

static void model_fill(World* w, uint32_t id, uint32_t value) {
    for (uint32_t word = 0; word < PAGE_WORDS; word++)
        w->model[(size_t)id * PAGE_WORDS + word] = value;
}

static void record_fill(VkCommandBuffer cb, World* w, uint32_t arena_page, uint32_t pages,
                        uint32_t value) {
    vkCmdFillBuffer(cb, w->arena, (VkDeviceSize)arena_page * GUEST_PAGE,
                    (VkDeviceSize)pages * GUEST_PAGE, value);
    for (uint32_t i = 0; i < pages; i++)
        model_fill(w, w->table[arena_page + i], value);
}

static void record_read(VkCommandBuffer cb, World* w) {
    VkBufferCopy copy = {.size = w->arena_size};
    vkCmdCopyBuffer(cb, w->arena, w->rb.buf, 1, &copy);
}

// Words of the last arena readback that differ from the model.
static uint32_t compare_gpu(World* w) {
    uint32_t bad = 0;
    for (uint32_t page = 0; page < w->arena_pages; page++)
        for (uint32_t word = 0; word < PAGE_WORDS; word++) {
            const uint32_t got = w->rb.map[(size_t)page * PAGE_WORDS + word];
            const uint32_t want = w->model[(size_t)w->table[page] * PAGE_WORDS + word];
            if (got != want && !bad++ && !w->detail[0])
                snprintf(w->detail, sizeof w->detail,
                         "GPU arena page %u word %u: %#x, expected %#x (page id %u)", page, word,
                         got, want, w->table[page]);
        }
    return bad;
}

// Words of guest memory, as the CPU reads both stores, that differ from the model.
static uint32_t compare_cpu(World* w) {
    uint32_t bad = 0;
    for (uint32_t s = 0; s < 2; s++)
        for (size_t at = 0; at < (size_t)w->phys_pages * PAGE_WORDS; at++) {
            const uint32_t got = w->store[s].cpu[at];
            const uint32_t want = w->model[(size_t)s * w->phys_pages * PAGE_WORDS + at];
            if (got != want && !bad++ && !w->detail[0])
                snprintf(w->detail, sizeof w->detail,
                         "CPU store %u page %zu word %zu: %#x, expected %#x", s, at / PAGE_WORDS,
                         at % PAGE_WORDS, got, want);
        }
    return bad;
}

// A GPU read of the arena in a submission of its own, and the CPU view, against the model.
static uint32_t verify(World* w) {
    VkCommandBuffer cb = begin();
    record_read(cb, w);
    barrier_all(cb);
    submit_wait(cb);
    return compare_gpu(w) + compare_cpu(w);
}

// GPU fill in one submission, then verify() in another: the strongest synchronization.
static uint32_t fill_verify(World* w, uint32_t arena_page, uint32_t pages, uint32_t value) {
    VkCommandBuffer cb = begin();
    record_fill(cb, w, arena_page, pages, value);
    barrier_all(cb);
    submit_wait(cb);
    return verify(w);
}

// GPU fill, a memory barrier, and the read of the whole arena in one command buffer: what a
// frame does when a write through one object is followed by a read through another.
static uint32_t fill_read_one_submit(World* w, uint32_t arena_page, uint32_t pages,
                                     uint32_t value) {
    VkCommandBuffer cb = begin();
    record_fill(cb, w, arena_page, pages, value);
    barrier_all(cb);
    record_read(cb, w);
    barrier_all(cb);
    submit_wait(cb);
    return compare_gpu(w) + compare_cpu(w);
}

static void cpu_fill(World* w, uint32_t store, uint32_t page, uint32_t value) {
    for (uint32_t word = 0; word < PAGE_WORDS; word++)
        w->store[store].cpu[(size_t)page * PAGE_WORDS + word] = value;
    model_fill(w, store * w->phys_pages + page, value);
}

// Negative control. Arena block copy_block is bound to the private copy of the guest pages
// that start at page original_page. Counts the words of the last readback of that block that
// differ from what an alias of the original pages would show. After a write to the original
// through any other view, a detector that works reports every written word here.
static uint32_t copy_differs(World* w, uint32_t copy_block, uint32_t original_page) {
    uint32_t differing = 0;
    for (uint32_t i = 0; i < w->r * PAGE_WORDS; i++)
        differing += w->rb.map[(size_t)copy_block * w->r * PAGE_WORDS + i] !=
                     w->model[(size_t)original_page * PAGE_WORDS + i];
    return differing;
}

static const char* first_detail(World* w) {
    return w->detail[0] ? w->detail : "no mismatch";
}

static void shifted_tests(int control, VkDeviceSize block, uint32_t store_blocks) {
    World world, *w = &world;
    const char* why = NULL;
    const int ready =
        world_init(w, control, block, store_blocks, (uint32_t)(block / GUEST_PAGE) + 4, &why);
    if (ready <= 0) {
        result("T1", ready, "shifted %s: %s", w->name, why);
        world_free(w);
        return;
    }
    const uint32_t r = w->r, base = 4 * r, common = base + r - 1;
    const long long gtt_before = gtt_used();
    int ok = 1;
    // Arena block p shows guest pages [base + p, base + p + r): r views of overlapping pages,
    // one per phase. Page `common` is in all of them, at page r - 1 - p of block p.
    for (uint32_t p = 0; ok && p < r; p++)
        ok = map_run(w, p, 1, 0, base + p, &why);
    // A two-block mapping with phase 1: virtually and physically contiguous (CE1).
    ok = ok && map_run(w, r, 2, 0, 8 * r + 1, &why);
    // Positive control: a second view of block 0 through the same memory object and offset.
    ok = ok && map_run(w, r + 2, 1, 0, base, &why);
    // Negative control: the same bytes on other pages.
    ok = ok && map_run(w, r + 3, 1, 1, base, &why);
    ok = ok && flush_binds(w, &why);
    if (!ok) {
        result("T1", 0, "shifted %s: %s", w->name, why);
        world_free(w);
        return;
    }
    const long long gtt_after = gtt_used();
    printf("    bind: %u blocks in %.3f ms; memory objects over store 0: %u; gtt_used %+lld "
           "bytes for a store of %llu bytes\n",
           r + 4, w->bind_ms, control ? 1 : r, gtt_after - gtt_before,
           (unsigned long long)w->store_size);

    uint32_t bad = verify(w);
    result("T1", bad == 0,
           "shifted %s: GPU reads %u views of the same pages at phases 0..%u KiB, a 2-block "
           "mapping at phase 16 KiB and both controls (%u mismatches; %s)",
           w->name, r, (r - 1) * 16, bad, first_detail(w));

    // The kernel accounts a memory object when a submission first uses it, not at the bind.
    printf("    after the first GPU use: gtt_used %+lld bytes; %u memory objects of %llu bytes "
           "over store 0, 1 over store 1\n",
           gtt_used() - gtt_before, control ? 1 : r,
           (unsigned long long)(control ? w->store_size : w->store_size - block));

    // T2: a GPU write through each phase must be seen through every other phase and by the CPU.
    bad = 0;
    for (uint32_t s = 0; s < r; s++)
        bad += fill_verify(w, s * r + (r - 1 - s), 1, 0xF2000000u | (s << 8) | s);
    result("T2", bad == 0,
           "shifted %s: GPU fill of guest page %u through each of %u phases, read back through "
           "all views and by the CPU, separate submissions (%u mismatches; %s)",
           w->name, common, r, bad, first_detail(w));
    const uint32_t differing = copy_differs(w, r + 3, base);
    result("T3", differing == PAGE_WORDS,
           "shifted %s: negative control, a private copy of the same bytes shows none of the "
           "writes (%u of %u written words differ from an alias)",
           w->name, differing, PAGE_WORDS);

    // T4: the same in one command buffer, fill then read with a memory barrier in between.
    bad = 0;
    for (uint32_t s = 0; s < r; s++)
        bad += fill_read_one_submit(w, s * r + (r - 1 - s), 1, 0xF4000000u | (s << 8) | s);
    result("T4", bad == 0,
           "shifted %s: the same with fill, barrier and read in one command buffer "
           "(%u mismatches; %s)",
           w->name, bad, first_detail(w));

    // T5: CPU writes after the binds: the common page, and a page of the phase-1 mapping.
    cpu_fill(w, 0, common, 0xC5000001u);
    cpu_fill(w, 0, 9 * r + 1, 0xC5000002u);
    bad = verify(w);
    result("T5", bad == 0,
           "shifted %s: CPU writes seen by the GPU through all views "
           "(%u mismatches; %s)",
           w->name, bad, first_detail(w));

    // T6: a GPU write across the block boundary inside the phase-1 mapping: arena pages
    // r*r + r - 1 and r*r + r are guest pages 9r and 9r + 1, neighbours in both spaces.
    bad = fill_verify(w, r * r + r - 1, 2, 0xF6000000u);
    result("T6", bad == 0,
           "shifted %s: GPU fill across a block boundary of the phase-16 KiB mapping lands on "
           "neighbouring guest pages %u and %u (%u mismatches; %s)",
           w->name, 9 * r, 9 * r + 1, bad, first_detail(w));
    world_free(w);
}

static void stitched_tests(int control, VkDeviceSize block, uint32_t store_blocks) {
    World world, *w = &world;
    const char* why = NULL;
    const int ready = world_init(w, control, block, store_blocks, 6, &why);
    if (ready <= 0) {
        result("U1", ready, "stitched %s: %s", w->name, why);
        world_free(w);
        return;
    }
    const uint32_t r = w->r;
    Piece* pieces = calloc(3 * r, sizeof(Piece));
    for (uint32_t i = 0; i < r; i++) {
        // Block 0: every page from another block and another phase, in descending order.
        pieces[i] = (Piece){0, (20 - 3 * i) * r + (3 * i + 1) % r};
        // Block 1: two physical runs that meet in the middle of the block (CE2).
        pieces[r + i] = (Piece){0, i < r / 2 ? 12 * r + i : 15 * r + 1 + (i - r / 2)};
        // Block 2: consecutive pages. Positive control: the same as an ordinary import.
        pieces[2 * r + i] = (Piece){0, 6 * r + i};
    }
    const long long gtt_before = gtt_used();
    int ok = map_stitched(w, 0, pieces, 3, &why);
    // Ordinary views of pages that the stitched object also holds.
    ok = ok && map_run(w, 3, 1, 0, 20 * r, &why); // holds pieces[0], page 20r + 1
    ok = ok && map_run(w, 4, 1, 0, 6 * r, &why);  // the pages of stitched block 2
    ok = ok && map_run(w, 5, 1, 1, 6 * r, &why);  // negative control: private copy
    ok = ok && flush_binds(w, &why);
    if (!ok) {
        result("U1", 0, "stitched %s: %s", w->name, why);
        free(pieces);
        world_free(w);
        return;
    }
    printf("    bind: 6 blocks in %.3f ms; gtt_used %+lld bytes\n", w->bind_ms,
           gtt_used() - gtt_before);

    uint32_t bad = verify(w);
    printf("    after the first GPU use: gtt_used %+lld bytes\n", gtt_used() - gtt_before);
    result("U1", bad == 0,
           "stitched %s: GPU reads 3 blocks built from guest pages %u,%u,.. (irregular), two "
           "runs, and a consecutive run, next to ordinary views (%u mismatches; %s)",
           w->name, pieces[0].page, pieces[1].page, bad, first_detail(w));

    // U2: GPU writes through every page of the two irregular blocks.
    bad = 0;
    for (uint32_t i = 0; i < 2 * r; i++)
        bad += fill_verify(w, i, 1, 0xE2000000u | i);
    result("U2", bad == 0,
           "stitched %s: GPU fill through each of %u stitched pages lands on its guest page, "
           "seen by the CPU and through the ordinary view (%u mismatches; %s)",
           w->name, 2 * r, bad, first_detail(w));

    // U3: GPU writes through the ordinary views, read through the stitched object.
    bad = fill_verify(w, 3 * r, r, 0xE3000001u) + fill_verify(w, 4 * r, r, 0xE3000002u);
    result("U3", bad == 0,
           "stitched %s: GPU fill through ordinary views is read through the stitched object "
           "(%u mismatches; %s)",
           w->name, bad, first_detail(w));
    const uint32_t differing = copy_differs(w, 5, 6 * r);
    result("U4", differing == r * PAGE_WORDS,
           "stitched %s: negative control, a private copy of the same bytes shows none of the "
           "writes (%u of %u written words differ from an alias)",
           w->name, differing, r * PAGE_WORDS);

    // U5: fill and read in one command buffer, in both directions.
    bad = fill_read_one_submit(w, 2 * r + 1, 1, 0xE5000001u) +
          fill_read_one_submit(w, 3 * r + 1, 1, 0xE5000002u) +
          fill_read_one_submit(w, 0, 1, 0xE5000003u);
    result("U5", bad == 0,
           "stitched %s: the same with fill, barrier and read in one command buffer "
           "(%u mismatches; %s)",
           w->name, bad, first_detail(w));

    // U6: exact partial unmap (CE4). Guest page pieces[1] leaves block 0; a scratch page takes
    // its place in a new object, and one rebind of the block swaps the objects. A GPU write
    // to the whole block must then leave the unmapped guest page alone.
    const uint32_t unmapped = pieces[1].page;
    pieces[1] = (Piece){1, 0};
    ok = map_stitched(w, 0, pieces, 1, &why) && flush_binds(w, &why);
    if (!ok) {
        result("U6", 0, "stitched %s: rebuild after unmap: %s", w->name, why);
    } else {
        printf("    rebind of one block after a partial unmap: %.3f ms\n", w->bind_ms);
        bad = verify(w) + fill_verify(w, 0, r, 0xE6000000u);
        result("U6", bad == 0,
               "stitched %s: guest page %u unmapped from a bound block by swapping in a new "
               "object; a GPU fill of the block leaves that page untouched (%u mismatches; %s)",
               w->name, unmapped, bad, first_detail(w));
    }
#ifdef HAVE_UDMABUF
    if (!control) {
        // Information: can one object hold the same guest page twice (an alias inside a block)?
        const Piece twice[4] = {{0, 2}, {0, 9}, {0, 2}, {0, 30}};
        struct udmabuf_create_list* list =
            calloc(1, sizeof(*list) + 4 * sizeof(struct udmabuf_create_item));
        list->flags = UDMABUF_FLAGS_CLOEXEC;
        list->count = 4;
        for (int i = 0; i < 4; i++)
            list->list[i] = (struct udmabuf_create_item){.memfd = (uint32_t)w->store[0].memfd,
                                                         .offset = twice[i].page * GUEST_PAGE,
                                                         .size = GUEST_PAGE};
        int ctl = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
        int fd = ctl < 0 ? -1 : ioctl(ctl, UDMABUF_CREATE_LIST, list);
        printf("U7  INFO stitched dma-buf: a list that names one guest page twice: %s\n",
               fd < 0 ? strerror(errno) : "accepted by the kernel");
        if (fd >= 0)
            close(fd);
        if (ctl >= 0)
            close(ctl);
        free(list);
    }
#endif
    free(pieces);
    world_free(w);
}

// V: calls that break a rule on purpose. They prove nothing about the driver. Run under
// VK_LAYER_KHRONOS_validation, they show that the layer is loaded (V1) and that it checks
// sparse binds against the granularity the driver reports (V2), so that a silent layer in
// the T and U runs means something. The runner counts the messages; the probe cannot see
// them.
static void validation_canary(void) {
    // V1: a buffer without usage. Harmless for a driver, an error for the layer.
    VkBufferCreateInfo bad = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = GUEST_PAGE,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buffer = VK_NULL_HANDLE;
    const VkResult r = vkCreateBuffer(dev, &bad, NULL, &buffer);
    printf("V1  INFO vkCreateBuffer with usage 0 -> %d: the layer must report it\n", r);
    if (r == VK_SUCCESS)
        vkDestroyBuffer(dev, buffer, NULL);

    // V2: one 16 KiB page bound at a 16 KiB offset of a sparse buffer whose reported
    // granularity is larger. Only made where the real granularity is known to be smaller
    // than the reported one (RADV of Mesa 25.0 with radv_force_64k_sparse_alignment), so the
    // driver itself is not asked for anything it cannot do.
    if (sparse_family == UINT32_MAX) {
        printf("V2  SKIP no sparse binding queue\n");
        return;
    }
    uint32_t families[2] = {work_family, sparse_family};
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
        .size = (VkDeviceSize)1 << 20,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode =
            sparse_family == work_family ? VK_SHARING_MODE_EXCLUSIVE : VK_SHARING_MODE_CONCURRENT,
        .queueFamilyIndexCount = sparse_family == work_family ? 0 : 2,
        .pQueueFamilyIndices = families};
    VkBuffer arena;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &arena));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, arena, &req);
    const int forced =
        getenv("radv_force_64k_sparse_alignment") != NULL && strstr(driver_name, "radv") != NULL;
    if (req.alignment <= GUEST_PAGE || (!forced && !getenv("PROBE_CANARY_ANY_DRIVER"))) {
        printf("V2  SKIP reported granularity %llu KiB, forced by the RADV option: %s; a "
               "misaligned bind is only made where the driver is known to accept it\n",
               (unsigned long long)(req.alignment >> 10), forced ? "yes" : "no");
        vkDestroyBuffer(dev, arena, NULL);
        return;
    }
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.alignment,
                               .memoryTypeIndex = find_type(req.memoryTypeBits, 0)};
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
    VkSparseMemoryBind bind = {.resourceOffset = GUEST_PAGE,
                               .size = GUEST_PAGE,
                               .memory = mem,
                               .memoryOffset = GUEST_PAGE};
    VkSparseBufferMemoryBindInfo bi = {.buffer = arena, .bindCount = 1, .pBinds = &bind};
    VkBindSparseInfo info = {
        .sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .bufferBindCount = 1, .pBufferBinds = &bi};
    const VkResult br = vkQueueBindSparse(sparse_queue, 1, &info, VK_NULL_HANDLE);
    vkQueueWaitIdle(sparse_queue);
    printf("V2  INFO bind of 16 KiB at offset 16 KiB with a reported granularity of %llu KiB "
           "-> %d: the layer must report it\n",
           (unsigned long long)(req.alignment >> 10), br);
    vkDestroyBuffer(dev, arena, NULL);
    vkFreeMemory(dev, mem, NULL);
}

int main(int argc, char** argv) {
    int device_index = 0;
    const char* backend = "auto";
    VkDeviceSize mib = 8, bw_mib = 64, block_kib = 64, store_mib = 2;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--device"))
            device_index = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--backend"))
            backend = argv[i + 1];
        else if (!strcmp(argv[i], "--mib"))
            mib = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--bw-mib"))
            bw_mib = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--block-kib"))
            block_kib = strtoull(argv[i + 1], NULL, 10);
        else if (!strcmp(argv[i], "--store-mib"))
            store_mib = strtoull(argv[i + 1], NULL, 10);
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

    if (!strcmp(backend, "validation-canary")) {
        validation_canary();
        vkDestroyCommandPool(dev, pool, NULL);
        vkDestroyDevice(dev, NULL);
        vkDestroyInstance(inst, NULL);
        return 0;
    }

    // The T and U tests run alone: --backend shifted-dmabuf, shifted-control, stitched-dmabuf,
    // stitched-control.
    if (!strncmp(backend, "shifted-", 8) || !strncmp(backend, "stitched-", 9)) {
        const int control = strstr(backend, "-control") != NULL;
        // The guest memory the imports cover. The tests touch the same pages whatever its
        // size; a larger store shows what the imports cost.
        const uint32_t store_blocks = (uint32_t)((store_mib << 10) / (block_kib ? block_kib : 1));
        if (!block_kib || store_blocks < 32 || store_mib > 1024) {
            fprintf(stderr, "--store-mib must give 32 blocks or more, and at most 1024 MiB\n");
            return 2;
        }
        if (backend[1] == 'h')
            shifted_tests(control, block_kib << 10, store_blocks);
        else
            stitched_tests(control, block_kib << 10, store_blocks);
        printf("summary: %d passed, %d failed\n", passes, failures);
        vkDestroyCommandPool(dev, pool, NULL);
        vkDestroyDevice(dev, NULL);
        vkDestroyInstance(inst, NULL);
        return failures ? 1 : 0;
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
