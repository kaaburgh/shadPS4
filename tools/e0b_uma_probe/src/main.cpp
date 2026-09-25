// SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// e0b_uma_probe: standalone feasibility probe for a shared (UMA) guest-memory buffer backend
// in shadPS4. See README.md for what each test answers and how to read the output.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <unistd.h>

#include "tests.h"

using namespace e0b;

static void Usage() {
    std::printf(
        "usage: e0b_uma_probe [options]\n"
        "  --list-devices              list Vulkan devices and exit\n"
        "  --device N                  use physical device N (default: first GPU)\n"
        "  --device-name SUBSTR        use the first device whose name contains SUBSTR\n"
        "  --allow-cpu                 allow CPU implementations (lavapipe) when picking by "
        "default\n"
        "  --validate                  enable VK_LAYER_KHRONOS_validation\n"
        "  --json PATH                 JSON report path (default: e0b-<host>-<time>.json)\n"
        "  --tests LIST                comma list of caps,t1,t2,t3,t4,t5,t6,t7 (default: all)\n"
        "  --backing-mib N             guest memfd size (default 512)\n"
        "  --arena-mib N               sparse arena / guest VA span (default 1024)\n"
        "  --t4-iters N                coherence stress iterations (default 256)\n"
        "  --t6-max N                  largest object count in T6 (default 4096)\n"
        "  --t6-iters N                submits per T6 measurement (default 64)\n"
        "  --seed N                    pattern/random seed\n"
        "  --force-type                bind imports even if the memory type is outside the\n"
        "                              arena's memoryTypeBits (invalid usage, diagnosis only)\n"
        "  --selftest-memfd-as-dmabuf  lavapipe self-test: hand out the memfd as a dma-buf\n");
}

static bool ParseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--list-devices") {
            o.list_devices = true;
        } else if (a == "--device") {
            o.device_index = std::stoi(next());
        } else if (a == "--device-name") {
            o.device_name = next();
        } else if (a == "--allow-cpu") {
            o.allow_cpu = true;
        } else if (a == "--validate") {
            o.validate = true;
        } else if (a == "--json") {
            o.json_path = next();
        } else if (a == "--tests") {
            std::stringstream ss(next());
            std::string t;
            while (std::getline(ss, t, ',')) {
                o.tests.insert(t);
            }
        } else if (a == "--backing-mib") {
            o.backing_mib = std::stoull(next());
        } else if (a == "--arena-mib") {
            o.arena_mib = std::stoull(next());
        } else if (a == "--t4-iters") {
            o.t4_iters = static_cast<uint32_t>(std::stoul(next()));
        } else if (a == "--t6-max") {
            o.t6_max = static_cast<uint32_t>(std::stoul(next()));
        } else if (a == "--t6-iters") {
            o.t6_iters = static_cast<uint32_t>(std::stoul(next()));
        } else if (a == "--seed") {
            o.seed = static_cast<uint32_t>(std::stoul(next(), nullptr, 0));
        } else if (a == "--force-type") {
            o.force_type = true;
        } else if (a == "--selftest-memfd-as-dmabuf") {
            o.fake_dmabuf = true;
        } else if (a == "-h" || a == "--help") {
            Usage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown option %s\n", a.c_str());
            Usage();
            return false;
        }
    }
    return true;
}

static std::string DefaultJsonPath() {
    char host[64] = {};
    gethostname(host, sizeof(host) - 1);
    char ts[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", std::localtime(&t));
    return std::string("e0b-") + host + "-" + ts + ".json";
}

int main(int argc, char** argv) {
    Options opt;
    if (!ParseArgs(argc, argv, opt)) {
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Report report;
    Context ctx;
    std::string err;
    if (!ctx.CreateInstance(opt.validate, &err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 2;
    }
    const auto devices = ctx.Devices();
    if (opt.list_devices) {
        for (size_t i = 0; i < devices.size(); ++i) {
            VkPhysicalDeviceProperties pr;
            vkGetPhysicalDeviceProperties(devices[i], &pr);
            std::printf("%zu: %s (type %d, api %u.%u)\n", i, pr.deviceName, pr.deviceType,
                        VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion));
        }
        return 0;
    }

    // Default: discrete GPU first, then integrated, then anything else; CPU only with --allow-cpu.
    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    int best_rank = 1 << 30;
    for (size_t i = 0; i < devices.size(); ++i) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(devices[i], &pr);
        if (opt.device_index >= 0 || !opt.device_name.empty()) {
            const bool match = opt.device_index >= 0
                                   ? static_cast<int>(i) == opt.device_index
                                   : std::strstr(pr.deviceName, opt.device_name.c_str()) != nullptr;
            if (match) {
                chosen = devices[i];
                break;
            }
            continue;
        }
        int rank = 3;
        switch (pr.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            rank = 0;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            rank = 1;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            rank = opt.allow_cpu ? 4 : -1;
            break;
        default:
            break;
        }
        if (rank >= 0 && rank < best_rank) {
            best_rank = rank;
            chosen = devices[i];
        }
    }
    if (!chosen) {
        std::fprintf(stderr, "no matching Vulkan device (try --list-devices / --allow-cpu)\n");
        return 2;
    }

    CollectEnvironment(report);
    if (!ctx.CreateDevice(chosen, opt, &err)) {
        std::fprintf(stderr, "device setup failed: %s\n", err.c_str());
        return 2;
    }

    GuestMemory gm;
    if (!gm.Create(opt.backing_mib * MiB, opt.arena_mib * MiB, &err)) {
        std::fprintf(stderr, "guest memory setup failed: %s\n", err.c_str());
        return 2;
    }
    DmaBufSource dbs;
    dbs.Init(gm, opt.fake_dmabuf);

    int rc = 0;
    {
        Probe probe(ctx, gm, dbs, report, opt);
        InitProbe(probe);
        probe.SetupArenas();
        probe.SetupExport();
        try {
            RunCapabilities(probe);
            RunFunctional(probe);
        } catch (const std::exception& e) {
            report.Add("probe", "setup", Status::Error, e.what());
        }
        if (!ctx.device_lost) {
            vkDeviceWaitIdle(ctx.device);
            probe.DestroyArenas();
        }
    }

    report.Section("run");
    report.Fact("validation_errors",
                opt.validate ? std::to_string(ctx.ValidationErrors()) : "not enabled");
    report.Fact("device_lost", ctx.device_lost ? "yes" : "no");
    report.PrintSummary();

    const std::string path = opt.json_path.empty() ? DefaultJsonPath() : opt.json_path;
    std::ofstream(path) << report.Json();
    std::printf("\nJSON report: %s\n", path.c_str());

    if (report.Count(Status::Fail) || report.Count(Status::Error)) {
        rc = 1;
    }
    if (ctx.device_lost) {
        std::fflush(stdout);
        std::_Exit(rc ? rc : 1); // skip teardown on a lost device
    }
    return rc;
}
