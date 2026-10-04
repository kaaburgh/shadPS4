// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace UmaCensus {
// All records are thirteen little-endian u64s. See tools/uma_e0/analyze.py.
enum class Kind : uint32_t {
    Map = 1,
    Piece,
    Unmap,
    Protect,
    Vma,
    BlockSize,
    Buffer,
    Snapshot,
    DmaSet,
    DmaRange,
    Session,
    SubmitSession,
    Submit,
    Complete,
    Packet,
    CpuWrite,
    Irq,
    Fault,
    Watch,
    Tracker,
    Readback,
    End,
    CommandBegin,
    Command,
    CommandEnd,
    Finish,
    ArenaBind,
    BackingWrite,
    Resident,
    Name,
    MemoryType,
    UnmapCall,
    GpuMap,
    GpuUnmap
};
struct Event {
    uint64_t seq, ns, thread, kind_flags, context, session, tick, address, size, a, b, c, cmd_seq;
};
static_assert(sizeof(Event) == 104);
inline std::atomic<bool> enabled{false};
inline bool Enabled() noexcept {
    return enabled.load(std::memory_order_relaxed);
}
uint64_t Now() noexcept;
uint64_t NewId() noexcept;
// Bounded, allocation-free, lock-free, errno-preserving producer; also used in faults.
void Emit(Kind kind, uint64_t address = 0, uint64_t size = 0, uint64_t context = 0,
          uint64_t session = 0, uint64_t tick = 0, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0,
          uint32_t flags = 0, uint64_t cmd_seq = 0) noexcept;
// Trivial initial-exec TLS, used only by normal-context backing-write origin hooks.
inline thread_local uint64_t origin_tag{}, origin_packet{};
struct OriginScope {
    uint64_t previous, previous_packet;
    explicit OriginScope(uint64_t tag, uint64_t packet = 0)
        : previous{origin_tag}, previous_packet{origin_packet} {
        origin_tag = tag;
        origin_packet = packet;
    }
    ~OriginScope() {
        origin_tag = previous;
        origin_packet = previous_packet;
    }
};
// Normal-context functions only.
bool Initialize();
void Metadata(const std::string& key, const std::string& value);
void SetObserverShutdown(void (*stop)());
void Drop() noexcept;
void Shutdown();
int SelfTest();
} // namespace UmaCensus
