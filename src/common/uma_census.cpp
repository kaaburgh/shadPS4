// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <nlohmann/json.hpp>
#include "common/scm_rev.h"
#include "common/uma_census.h"
#ifdef __linux__
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#endif

namespace UmaCensus {
namespace {
constexpr uint64_t Capacity = 1 << 16;
struct Slot {
    std::atomic<uint64_t> ticket{};
    Event event{};
};
// Static storage intentionally survives shutdown and late/nested faults.
std::array<Slot, Capacity> slots;
std::atomic<uint64_t> head{0}, dropped{0}, next_id{1}, written{0}, active{0};
std::atomic<bool> stopping{false}, accepting{false};
std::jthread collector;
std::mutex metadata_mutex;
nlohmann::json metadata;
std::filesystem::path directory;
FILE* output{};
uint64_t limit_bytes = 512ULL << 20;
uint64_t start_ns{}, tail{};
void (*stop_observers)(){};

void SaveMetadata(bool final) {
    std::scoped_lock lock{metadata_mutex};
    metadata["dropped_events"] = dropped.load();
    metadata["written_events"] = written.load();
    metadata["reserved_events"] = head.load();
    metadata["capture_end_monotonic_ns"] = Now();
    metadata["capture_end_unix_ns"] = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count();
    metadata["clean_shutdown"] = final;
    auto temporary = directory / "metadata.json.tmp";
    std::ofstream file(temporary);
    file << metadata.dump(2) << '\n';
    file.close();
    std::filesystem::rename(temporary, directory / "metadata.json");
}
void Collect() {
    uint64_t last_save = 0;
    for (;;) {
        size_t drained = 0;
        while (drained < 8192) {
            auto& slot = slots[tail % Capacity];
            if (slot.ticket.load(std::memory_order_acquire) != tail + 1)
                break;
            if ((written.load() + 1) * sizeof(Event) <= limit_bytes &&
                fwrite(&slot.event, sizeof(Event), 1, output) == 1) {
                written.fetch_add(1, std::memory_order_relaxed);
            } else {
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
            slot.ticket.store(tail + Capacity, std::memory_order_release);
            ++tail;
            ++drained;
        }
        if (Now() - last_save > 250000000 || stopping.load()) {
            fflush(output);
            SaveMetadata(false);
            last_save = Now();
        }
        if (stopping.load() && tail == head.load())
            break;
        if (drained == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    fclose(output);
    output = nullptr;
    SaveMetadata(true);
}
} // namespace
uint64_t Now() noexcept {
#ifdef __linux__
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#else
    return 0; // Linux research instrumentation only.
#endif
}
uint64_t NewId() noexcept {
    return next_id.fetch_add(1, std::memory_order_relaxed);
}
void Emit(Kind kind, uint64_t address, uint64_t size, uint64_t context, uint64_t session,
          uint64_t tick, uint64_t a, uint64_t b, uint64_t c, uint32_t flags,
          uint64_t cmd_seq) noexcept {
    if (!Enabled())
        return;
    active.fetch_add(1, std::memory_order_acquire);
    if (!Enabled()) {
        active.fetch_sub(1, std::memory_order_release);
        return;
    }
    const int saved_errno = errno;
    uint64_t position = head.load(std::memory_order_relaxed);
    bool reserved = false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto& slot = slots[position % Capacity];
        const auto ticket = slot.ticket.load(std::memory_order_acquire);
        if (ticket < position)
            break; // Full; never wait in a fault.
        if (ticket == position &&
            head.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
            reserved = true;
            break;
        }
        position = head.load(std::memory_order_relaxed);
    }
    if (reserved) {
        auto& slot = slots[position % Capacity];
        uint64_t tid = 0;
#ifdef __linux__
        tid = static_cast<uint64_t>(syscall(SYS_gettid));
#endif
        slot.event = {position, Now(),   tid,  uint64_t(kind) | (uint64_t(flags) << 32),
                      context,  session, tick, address,
                      size,     a,       b,    c,
                      cmd_seq};
        slot.ticket.store(position + 1, std::memory_order_release);
    } else {
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    active.fetch_sub(1, std::memory_order_release);
    errno = saved_errno;
}
bool Initialize() {
    const char* path = std::getenv("SHADPS4_UMA_E0_CAPTURE");
    if (!path || !*path)
        return true;
#if !defined(__linux__)
    std::fputs("UMA E0 instrumentation currently requires Linux\n", stderr);
    return false;
#else
    static_assert(std::atomic<uint64_t>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::endian::native == std::endian::little);
    directory = path;
    std::error_code error;
    if (!std::filesystem::create_directory(directory, error)) {
        std::fprintf(stderr, "UMA E0 requires a new capture directory: %s\n", path);
        return false;
    }
    output = fopen((directory / "events.bin").c_str(), "wb");
    if (!output)
        return false;
    if (const auto* budget = std::getenv("SHADPS4_UMA_E0_MAX_BYTES")) {
        char* end{};
        auto parsed = strtoull(budget, &end, 10);
        if (*end || parsed < sizeof(Event)) {
            fclose(output);
            output = nullptr;
            return false;
        }
        limit_bytes = parsed;
    }
    for (uint64_t i = 0; i < Capacity; ++i)
        slots[i].ticket.store(i);
    start_ns = Now();
    utsname host{};
    uname(&host);
    metadata = {{"schema", "shadps4-uma-e0/v1"},
                {"record_bytes", sizeof(Event)},
                {"byte_order", "little"},
                {"source_sha", UMA_E0_SOURCE_SHA},
                {"branch", std::string(Common::g_scm_branch)},
                {"source_description", std::string(Common::g_scm_desc)},
                {"build_configuration", UMA_E0_BUILD_CONFIGURATION},
                {"os", host.sysname},
                {"kernel", host.release},
                {"machine", host.machine},
                {"capture_start_monotonic_ns", start_ns},
                {"limit_bytes", limit_bytes},
                {"ring_capacity", Capacity},
                {"evidence", "exploratory-unverified"},
                {"capture_start_unix_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count()}};
    for (const char* key : {"SHADPS4_UMA_E0_HARNESS", "SHADPS4_UMA_E0_PARAMETERS",
                            "SHADPS4_UMA_E0_PATCH", "SHADPS4_UMA_E0_SOURCE_SHA"}) {
        metadata[key] = std::getenv(key) ? std::getenv(key) : "unspecified";
    }
    SaveMetadata(false);
    accepting.store(true);
    enabled.store(true);
    collector = std::jthread(Collect);
    std::atexit(Shutdown);
    std::at_quick_exit(Shutdown);
    return true;
#endif
}
void Metadata(const std::string& key, const std::string& value) {
    if (!Enabled())
        return;
    std::scoped_lock lock{metadata_mutex};
    metadata[key] = value;
}
void SetObserverShutdown(void (*stop)()) {
    stop_observers = stop;
}
void Drop() noexcept {
    dropped.fetch_add(1, std::memory_order_relaxed);
}
void Shutdown() {
    if (!accepting.exchange(false))
        return;
    if (stop_observers)
        stop_observers();
    Emit(Kind::End);
    enabled.store(false);
    while (active.load(std::memory_order_acquire))
        std::this_thread::yield();
    stopping.store(true);
    if (collector.joinable())
        collector.join();
}
int SelfTest() {
    if (!Initialize())
        return 1;
    if (!Enabled()) {
        const auto start = Now();
        for (int i = 0; i < 1000000; ++i)
            Emit(Kind::Fault);
        std::printf("disabled Emit: %.3f ns/call (synthetic hook microbenchmark)\n",
                    double(Now() - start) / 1000000);
        return 0;
    }
    Metadata("evidence", "synthetic");
    Metadata("readbacks_mode", "synthetic");
#ifdef __linux__
    std::signal(SIGUSR2, +[](int) { Emit(Kind::Fault, 0x300000000ULL, 8, 0, 0, 0, 1); });
#endif
    auto producer = [] {
        for (int i = 0; i < 10000; ++i)
            Emit(Kind::Fault, 0x200000000ULL + i * 4096ULL, 8, 0, 0, 0, 0, 0, 0, 1);
    };
    std::array<std::jthread, 4> threads;
    for (auto& thread : threads)
        thread = std::jthread(producer);
    for (auto& thread : threads)
        thread.join();
#ifdef __linux__
    for (int i = 0; i < 1000; ++i)
        raise(SIGUSR2);
    std::signal(SIGUSR2, SIG_DFL);
#endif
    Shutdown();
    return 0;
}
} // namespace UmaCensus
