// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace Vulkan {
class Scheduler;
}

namespace VideoCore::Sync {
inline uint64_t TimelineTime() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
struct TimelineIdentity {};
class SubmittedTick;
class CompletedPrefix;
template <class Handle>
class SubmittedPrefix;
class SubmittedTick {
public:
    uint64_t SubmittedAt() const {
        return submitted_ns;
    }
    uint64_t Value() const {
        return value;
    }
    bool BelongsTo(const std::shared_ptr<const TimelineIdentity>& owner) const {
        return identity == owner;
    }

private:
    template <class Handle>
    friend class SubmittedPrefix;
    SubmittedTick(std::shared_ptr<const TimelineIdentity> owner, uint64_t tick, uint64_t timestamp)
        : identity{std::move(owner)}, value{tick}, submitted_ns{timestamp} {}
    std::shared_ptr<const TimelineIdentity> identity;
    uint64_t value, submitted_ns;
};
// A successful existing synchronous wait, separate from a submission receipt.
class CompletedPrefix {
public:
    const SubmittedTick& Ticket() const {
        return ticket;
    }
    uint64_t ObservedAt() const {
        return observed_ns;
    }

private:
    template <class Handle>
    friend class SubmittedPrefix;
    explicit CompletedPrefix(SubmittedTick accepted)
        : ticket{std::move(accepted)}, observed_ns{TimelineTime()} {}
    SubmittedTick ticket;
    uint64_t observed_ns;
};
// The production scheduler and fake-queue adapter tests share this exact detach /
// success-minting operation. A ticket cannot be constructed from CurrentTick.
template <class Handle>
class SubmittedPrefix {
public:
    struct Session {
        uint64_t census_id{};
        Handle upload{}, primary{};
    };
    using Result = std::expected<SubmittedTick, int32_t>;
    std::vector<Session>& Sessions() {
        return sessions;
    }
    const std::vector<Session>& Sessions() const {
        return sessions;
    }
    const auto& Identity() const {
        return identity;
    }
    void MarkAccessed() const {
        accessed = true;
    }
    bool KnownEmpty() const {
        return !accessed;
    }
    template <class Observe, class Submit, class Resume>
    Result SubmitCurrent(uint64_t tick, Observe&& observe, Submit&& submit, Resume&& resume) {
        std::vector<Handle> commands;
        commands.reserve(sessions.size() * 2);
        for (const auto& session : sessions) {
            observe(session);
            if (session.upload)
                commands.push_back(session.upload);
            commands.push_back(session.primary);
        }
        sessions.clear();
        accessed = false;
        const auto submitted_ns = TimelineTime();
        const int32_t result = submit(std::span<const Handle>{commands});
        if (result != 0)
            return std::unexpected(result);
        SubmittedTick accepted{identity, tick, submitted_ns};
        resume(); // Fresh suffix; commands created here cannot enter accepted prefix.
        return accepted;
    }

    template <class Wait>
    std::expected<CompletedPrefix, int32_t> ObserveCompletion(const SubmittedTick& ticket,
                                                              Wait&& wait) {
        if (!ticket.BelongsTo(identity))
            return std::unexpected(-1);
        const int32_t status = wait(ticket.Value());
        if (status != 0)
            return std::unexpected(status);
        return CompletedPrefix{ticket};
    }

private:
    std::shared_ptr<const TimelineIdentity> identity = std::make_shared<TimelineIdentity>();
    std::vector<Session> sessions;
    mutable bool accessed{}; // Recorder-thread owned; conservative command-buffer-access proxy.
};
} // namespace VideoCore::Sync
