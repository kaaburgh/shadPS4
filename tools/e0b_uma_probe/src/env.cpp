// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Host facts that decide whether shared guest memory can work: kernel, udmabuf limits and
// permissions, swiotlb, GTT/TTM limits, memlock, driver versions.

#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "common.h"
#include "guest_mem.h"
#include "report.h"

namespace e0b {

static void FactFile(Report& r, const std::string& key, const std::string& path) {
    std::string err;
    const std::string v = ReadFileTrim(path, &err);
    r.Fact(key, v.empty() && !err.empty() ? "<" + err + ">" : v);
}

static std::string FirstLineWith(const std::string& path, const std::string& prefix) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(prefix, 0) == 0) {
            return line;
        }
    }
    return {};
}

static std::vector<std::string> ListDir(const std::string& path) {
    std::vector<std::string> v;
    if (DIR* d = opendir(path.c_str())) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] != '.') {
                v.emplace_back(e->d_name);
            }
        }
        closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}

void CollectEnvironment(Report& r) {
    r.Section("environment");
    utsname u{};
    uname(&u);
    r.Fact("kernel.release", u.release);
    r.Fact("kernel.version", u.version);
    r.Fact("kernel.machine", u.machine);
    FactFile(r, "kernel.cmdline", "/proc/cmdline");
    r.Fact("os", FirstLineWith("/etc/os-release", "PRETTY_NAME="));
    r.Fact("cpu", FirstLineWith("/proc/cpuinfo", "model name"));
    r.Fact("mem.total", FirstLineWith("/proc/meminfo", "MemTotal:"));

    rlimit rl{};
    getrlimit(RLIMIT_MEMLOCK, &rl);
    auto lim = [](rlim_t v) {
        return v == RLIM_INFINITY ? std::string("unlimited") : std::to_string(v / KiB) + " KiB";
    };
    r.Fact("rlimit.memlock", lim(rl.rlim_cur) + " (hard " + lim(rl.rlim_max) + ")");

    // udmabuf: device node, permissions, module parameters.
    struct stat st {};
    if (stat("/dev/udmabuf", &st) == 0) {
        r.Fact("udmabuf.node",
               Sprintf("mode %o uid %u gid %u, access(rw)=%s", st.st_mode & 07777, st.st_uid,
                       st.st_gid, access("/dev/udmabuf", R_OK | W_OK) == 0 ? "yes" : "no"));
    } else {
        r.Fact("udmabuf.node", "<missing>");
    }
    FactFile(r, "udmabuf.size_limit_mb", "/sys/module/udmabuf/parameters/size_limit_mb");
    FactFile(r, "udmabuf.list_limit", "/sys/module/udmabuf/parameters/list_limit");

    // swiotlb bouncing would break coherence of imported pages; debugfs usually needs root.
    FactFile(r, "swiotlb.io_tlb_used", "/sys/kernel/debug/swiotlb/io_tlb_used");
    FactFile(r, "swiotlb.io_tlb_nslabs", "/sys/kernel/debug/swiotlb/io_tlb_nslabs");
    r.Fact("iommu.groups", std::to_string(ListDir("/sys/kernel/iommu_groups").size()));
    FactFile(r, "thp.shmem_enabled", "/sys/kernel/mm/transparent_hugepage/shmem_enabled");

    // GPU drivers.
    r.Fact("nvidia.version", FirstLineWith("/proc/driver/nvidia/version", "NVRM"));
    FactFile(r, "nvidia.module_version", "/sys/module/nvidia/version");
    FactFile(r, "amdgpu.gttsize", "/sys/module/amdgpu/parameters/gttsize");
    FactFile(r, "ttm.pages_limit", "/sys/module/ttm/parameters/pages_limit");
    for (const auto& card : ListDir("/sys/class/drm")) {
        if (card.rfind("card", 0) != 0 || card.find('-') != std::string::npos) {
            continue;
        }
        const std::string dev = "/sys/class/drm/" + card + "/device/";
        char link[256] = {};
        const ssize_t n = readlink((dev + "driver").c_str(), link, sizeof(link) - 1);
        std::string drv = n > 0 ? std::string(link, static_cast<size_t>(n)) : "?";
        drv = drv.substr(drv.find_last_of('/') + 1);
        r.Fact(card + ".driver", drv + " vendor " + ReadFileTrim(dev + "vendor") + " device " +
                                     ReadFileTrim(dev + "device"));
        for (const char* f : {"mem_info_gtt_total", "mem_info_gtt_used", "mem_info_vram_total",
                              "mem_info_vis_vram_total"}) {
            const std::string v = ReadFileTrim(dev + f);
            if (!v.empty()) {
                r.Fact(card + "." + f, Sprintf("%llu MiB", std::stoull(v) / MiB));
            }
        }
    }
}

} // namespace e0b
