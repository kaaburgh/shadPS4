// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include "video_core/synchronization/submitted_prefix.h"

namespace VideoCore::Sync {
inline uint64_t CompletionTime() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
enum class ScalarFamily { Eop, Eos, Release };
enum class ScalarData { None, Immediate32, Immediate64, Clock, Counter };
// Entirely owned. Numeric guest VA is resolved and checked on the CP at publication;
// the guest must retain the destination allocation until its completion is published.
struct ScalarCompletion {
    ScalarFamily family{};
    ScalarData data{};
    uint64_t address{}, value{}, packet_id{}, parsed_ns{}, submitted_ns{};
    uint32_t queue{}, irq{}, event_control{};
    bool interrupt{}, empty_prefix{}, synchronous{};

    template <class Store, class Irq, class Clock, class Counter>
    bool Publish(Store&& store, Irq&& signal, Clock&& clock, Counter&& counter) const {
        uint64_t payload = value;
        uint32_t size = 8;
        switch (data) {
        case ScalarData::None:
            size = 0;
            break;
        case ScalarData::Immediate32:
            size = 4;
            break;
        case ScalarData::Immediate64:
            break;
        case ScalarData::Clock:
            payload = clock();
            break;
        case ScalarData::Counter:
            payload = counter();
            break;
        }
        if (size && !store(address, payload, size, family == ScalarFamily::Release))
            return false; // Never publish a success IRQ for a failed destination store.
        if (interrupt)
            signal(irq);
        return true;
    }
};
enum class WaitStatus { Completed, Cancelled, Failed };
// One dedicated completion lane, never the scheduler's general deferred-operation worker.
// The wait adapter MUST use finite waits on accepted tickets. Wake only posts a CP message.
class GuestCompletionLane {
public:
    struct Ready {
        SubmittedTick ticket;
        ScalarCompletion action;
        uint64_t sequence{}, observed_ns{}, ready_ns{};
    };
    using Wait = std::function<WaitStatus(const SubmittedTick&, std::stop_token)>;
    using Wake = std::function<void()>;
    GuestCompletionLane(std::shared_ptr<const TimelineIdentity> identity, Wait wait, Wake wake)
        : identity{std::move(identity)}, wait{std::move(wait)}, wake{std::move(wake)},
          worker{[this](std::stop_token stop) { Run(stop); }} {}
    ~GuestCompletionLane() {
        Stop();
    }
    GuestCompletionLane(const GuestCompletionLane&) = delete;
    bool Enqueue(const SubmittedTick& ticket, ScalarCompletion action) {
        return Admit(ticket, action, false);
    }
    bool Enqueue(const CompletedPrefix& proof, ScalarCompletion action) {
        return Admit(proof.Ticket(), action, true);
    }

private:
    bool Admit(const SubmittedTick& ticket, ScalarCompletion action, bool completed) {
        std::unique_lock lock{mutex};
        if (!accepting || !ticket.BelongsTo(identity) || depth == MaxPending)
            return false;
        auto& order = queues[action.queue];
        Ready ready{ticket, action, order.issued++, 0, 0};
        ++depth;
        high_water = std::max(high_water, depth);
        if (completed) {
            ready.observed_ns = ready.ready_ns = CompletionTime();
            order.ready.emplace(ready.sequence, std::move(ready));
            lock.unlock();
            wake();
        } else {
            pending.push_back(std::move(ready));
            cv.notify_one();
        }
        return true;
    }

public:
    // CP only. Remove under the lane lock, publish without it. Per-queue sequence gates
    // also cover synchronous proofs that arrive ahead of an older asynchronous action.
    template <class Publish>
    void Drain(Publish&& publish) {
        for (;;) {
            std::optional<Ready> item;
            {
                std::scoped_lock lock{mutex};
                if (!accepting)
                    return;
                for (auto& [qid, order] : queues) {
                    auto entry = order.ready.find(order.published);
                    if (entry == order.ready.end())
                        continue;
                    item.emplace(std::move(entry->second));
                    order.ready.erase(entry);
                    ++order.published;
                    --depth;
                    break;
                }
            }
            if (!item)
                return;
            publish(*item);
        }
    }
    // CP after an existing successful synchronous Finish. This releases older accepted
    // jobs too, without another GPU wait, before the synchronous GDS store is performed.
    void CompleteThrough(const CompletedPrefix& completion) {
        const auto& proof = completion.Ticket();
        std::scoped_lock lock{mutex};
        if (!accepting || !proof.BelongsTo(identity))
            return;
        completed_through = std::max(completed_through, proof.Value());
        if (active && active->ticket.Value() <= completed_through) {
            active->observed_ns = active->ready_ns = CompletionTime();
            queues[active->action.queue].ready.emplace(active->sequence, *active);
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->ticket.Value() > completed_through) {
                ++it;
                continue;
            }
            it->observed_ns = it->ready_ns = CompletionTime();
            queues[it->action.queue].ready.emplace(it->sequence, std::move(*it));
            it = pending.erase(it);
        }
    }
    // May be called again. The caller stops admission/invalidates CP delivery first.
    void Stop() {
        {
            std::scoped_lock lock{mutex};
            accepting = false;
            pending.clear();
            queues.clear();
            cancelled += depth;
            depth = 0;
            active.reset();
        }
        worker.request_stop();
        cv.notify_all();
        if (worker.joinable())
            worker.join();
    }
    size_t CancelledCount() const {
        std::scoped_lock lock{mutex};
        return cancelled;
    }
    size_t HighWater() const {
        std::scoped_lock lock{mutex};
        return high_water;
    }
    bool Failed() const {
        std::scoped_lock lock{mutex};
        return failed;
    }

private:
    static constexpr size_t MaxPending = 65536;
    struct Order {
        uint64_t issued{}, published{};
        std::map<uint64_t, Ready> ready;
    };
    void Run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::optional<Ready> job;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, stop, [&] { return !pending.empty() || !accepting; });
                if (!accepting || stop.stop_requested())
                    return;
                job.emplace(std::move(pending.front()));
                pending.pop_front();
                active = job;
            }
            const auto status = wait(job->ticket, stop);
            bool notify = false;
            {
                std::scoped_lock lock{mutex};
                if (!accepting || stop.stop_requested())
                    return;
                active.reset();
                if (status == WaitStatus::Completed || job->ticket.Value() <= completed_through) {
                    job->observed_ns = CompletionTime();
                    job->ready_ns = CompletionTime();
                    auto& order = queues[job->action.queue];
                    if (job->sequence >= order.published)
                        order.ready.emplace(job->sequence, std::move(*job));
                    notify = true;
                } else {
                    failed = status == WaitStatus::Failed;
                    accepting = false;
                    pending.clear();
                    queues.clear();
                    depth = 0;
                    notify = failed;
                }
            }
            if (notify)
                wake();
            if (status != WaitStatus::Completed)
                return;
        }
    }
    std::shared_ptr<const TimelineIdentity> identity;
    Wait wait;
    Wake wake;
    mutable std::mutex mutex;
    std::condition_variable_any cv;
    std::deque<Ready> pending;
    std::optional<Ready> active;
    std::map<uint32_t, Order> queues;
    size_t depth{}, high_water{}, cancelled{};
    uint64_t completed_through{};
    bool accepting{true}, failed{};
    std::jthread worker;
};
// Generic CP blocking-wait adapter. Service messages only after releasing the VO
// mutex; finite sleeps also recover a wake racing the transition into the wait.
template <class Mutex, class Cv, class Predicate, class Pending, class Pump, class Stop>
void WaitWithProgress(Mutex& mutex, Cv& cv, Predicate&& pred, Pending&& pending, Pump&& pump,
                      Stop&& stop) {
    while (!stop()) {
        {
            std::unique_lock lock{mutex};
            if (pred())
                return;
            cv.wait_for(lock, std::chrono::milliseconds{5},
                        [&] { return pred() || pending() || stop(); });
        }
        pump();
    }
}
} // namespace VideoCore::Sync
