// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/amdgpu/scalar_completion.h"
#include "video_core/synchronization/cp_completion_target.h"
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
using namespace VideoCore::Sync;
using namespace std::chrono_literals;
namespace Libraries::Kernel {
u64 PS4_SYSV_ABI sceKernelGetTscFrequency() { return 1000000; }
u64 PS4_SYSV_ABI sceKernelReadTsc() { return 1000; }
} // namespace Libraries::Kernel
namespace Libraries::GnmDriver {
u32 PS4_SYSV_ABI sceGnmGetGpuCoreClockFrequency() { return 1000; }
} // namespace Libraries::GnmDriver
struct Fixture {
  SubmittedPrefix<int> prefix;
  std::atomic<uint64_t> completed{};
  std::atomic<int> waits{}, wakes{};
  std::mutex wake_mutex;
  std::condition_variable wake_cv;
  std::unique_ptr<GuestCompletionLane> lane;
  std::vector<int> published;
  Fixture() {
    lane = std::make_unique<GuestCompletionLane>(
        prefix.Identity(),
        [&](const auto &ticket, std::stop_token stop) {
          ++waits;
          while (!stop.stop_requested()) {
            if (completed >= ticket.Value())
              return WaitStatus::Completed;
            std::this_thread::sleep_for(1ms);
          }
          return WaitStatus::Cancelled;
        },
        [&] {
          ++wakes;
          wake_cv.notify_all();
        });
  }
  SubmittedTick Submit(uint64_t tick) {
    prefix.Sessions().push_back({tick, 0, int(tick)});
    return *prefix.SubmitCurrent(
        tick, [](auto &) {}, [](auto) { return 0; }, [] {});
  }
  void Pump() {
    lane->Drain([&](const auto &ready) {
      const auto owner = std::this_thread::get_id();
      assert(ready.action.Publish(
          [&](auto, auto value, auto, bool) {
            assert(std::this_thread::get_id() == owner);
            published.push_back(int(value));
            return true;
          },
          [&](auto) { published.push_back(-1); }, [] { return 100ull; },
          [] { return 200ull; }));
    });
  }
  void Await(int number) {
    auto end = std::chrono::steady_clock::now() + 2s;
    while (wakes < number && std::chrono::steady_clock::now() < end)
      std::this_thread::sleep_for(1ms);
    assert(wakes >= number);
  }
};
int main() {
  using namespace AmdGpu;
  for (auto family :
       {ScalarFamily::Eop, ScalarFamily::Eos, ScalarFamily::Release}) {
    Fixture f;
    auto ticket = f.Submit(1);
    ScalarCompletion a{.family = family,
                       .data = ScalarData::Immediate32,
                       .address = 0x1000,
                       .value = 42,
                       .queue = 1,
                       .interrupt = family != ScalarFamily::Eos};
    assert(f.lane->Enqueue(ticket, a));
    a.value = 99; // Payload remains owned, independent of caller storage.
    f.Pump();
    assert(f.published.empty());
    f.completed = 1;
    f.Await(1);
    f.Pump();
    assert(f.published[0] == 42);
    if (family != ScalarFamily::Eos)
      assert(f.published == std::vector<int>({42, -1}));
    // B need not be submitted/completed to publish A.
    f.prefix.Sessions().push_back({2, 0, 2});
    assert(f.completed == 1);
  }
  {
    Fixture f;
    auto a = f.Submit(1), b = f.Submit(2);
    assert(f.lane->Enqueue(
        a, {.data = ScalarData::Immediate32, .value = 1, .queue = 5}));
    // Existing Finish proof for B arrives before the asynchronous A
    // observation.
    auto proof = *f.prefix.ObserveCompletion(b, [](auto) { return 0; });
    assert(f.lane->Enqueue(
        proof, {.data = ScalarData::Immediate32, .value = 2, .queue = 5}));
    f.Pump();
    assert(f.published.empty());
    f.lane->CompleteThrough(proof);
    f.Pump();
    assert(f.published == std::vector<int>({1, 2}));
    f.completed = 2;
    std::this_thread::sleep_for(10ms);
    f.Pump();
    assert(f.published.size() == 2);
  }
  {
    Fixture f;
    auto t = f.Submit(1);
    SubmittedPrefix<int> other;
    auto wrong =
        *other.SubmitCurrent(1, [](auto &) {}, [](auto) { return 0; }, [] {});
    assert(!f.lane->Enqueue(wrong, {}));
    f.lane->Enqueue(t, {});
    auto begin = std::chrono::steady_clock::now();
    f.lane->Stop();
    assert(std::chrono::steady_clock::now() - begin < 250ms);
    assert(!f.lane->Enqueue(t, {}));
    f.Pump();
    assert(f.published.empty());
  }
  {
    Fixture f;
    auto t = f.Submit(1);
    f.completed = 1;
    f.lane->Enqueue(t, {.value = 9});
    f.Await(1);
    f.lane->Stop();
    f.Pump();
    assert(f.published.empty()); // ready teardown invalidation
  }
  {
    SubmittedPrefix<int> prefix;
    auto failure = prefix.SubmitCurrent(
        1, [](auto &) {}, [](auto) { return -4; }, [] { assert(false); });
    assert(!failure); // No ticket, thus no possible admission.
    auto ticket =
        *prefix.SubmitCurrent(2, [](auto &) {}, [](auto) { return 0; }, [] {});
    std::atomic<int> wakes{};
    GuestCompletionLane lane(
        prefix.Identity(), [](auto &, auto) { return WaitStatus::Failed; },
        [&] { ++wakes; });
    assert(lane.Enqueue(ticket, {.value = 1}));
    for (int i = 0; i < 1000 && !wakes; ++i)
      std::this_thread::sleep_for(1ms);
    assert(lane.Failed());
    lane.Drain([](auto &) { assert(false); });
  }
  { // VO mutex is released for message servicing; idle CP needs no later
    // submit.
    Fixture f;
    auto t = f.Submit(1);
    bool label = false;
    std::mutex vo_mutex;
    std::condition_variable vo_cv;
    f.lane->Enqueue(t, {.data = ScalarData::Immediate32, .value = 1});
    std::jthread gpu([&] {
      std::this_thread::sleep_for(10ms);
      f.completed = 1;
      vo_cv.notify_all();
    });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    WaitWithProgress(
        vo_mutex, vo_cv, [&] { return label; }, [&] { return f.wakes > 0; },
        [&] {
          assert(vo_mutex.try_lock());
          vo_mutex.unlock();
          f.Pump();
          label = !f.published.empty();
        },
        [&] { return std::chrono::steady_clock::now() > deadline; });
    assert(label);
  }
  { // Checked failed store must not signal an IRQ; clock sampling at
    // publication.
    ScalarCompletion a{.data = ScalarData::Clock, .interrupt = true};
    bool irq = false;
    int clock = 0;
    assert(!a.Publish([](auto...) { return false; }, [&](auto) { irq = true; },
                      [&] {
                        ++clock;
                        return 9ull;
                      },
                      [] { return 0ull; }));
    assert(!irq && clock == 1);
  }
  { // Actual packet-to-owned-descriptor adapter, every scalar selector.
    PM4CmdEventWriteEop p{.header = {PM4ItOpcode::EventWriteEop, 5}};
    p.address_lo = 0x1000;
    p.data_lo = 41;
    p.data_hi = 1;
    for (auto selector :
         {DataSelect::None, DataSelect::Data32Low, DataSelect::Data64,
          DataSelect::GpuClock64, DataSelect::PerfCounter}) {
      p.data_sel.Assign(selector);
      p.int_sel.Assign(InterruptSelect::IrqWhenWriteConfirm);
      auto a = OwnScalar(p);
      assert(a.address == 0x1000 && a.value == 0x100000029ull);
    }
    p.data_sel.Assign(DataSelect::None);
    p.int_sel.Assign(InterruptSelect::IrqOnly);
    auto a = OwnScalar(p);
    assert(a.interrupt && a.data == ScalarData::None);
    PM4CmdEventWriteEos eos{.header = {PM4ItOpcode::EventWriteEos, 4}};
    eos.command.Assign(PM4CmdEventWriteEos::Command::SignalFence);
    assert(OwnScalar(eos).data == ScalarData::Immediate32);
    PM4CmdReleaseMem release{.header = {PM4ItOpcode::ReleaseMem, 6}};
    for (auto selector : {DataSelect::Data32Low, DataSelect::Data64,
                          DataSelect::GpuClock64, DataSelect::PerfCounter}) {
      release.data_sel.Assign(selector);
      release.int_sel.Assign(InterruptSelect::IrqUndocumented);
      assert(OwnScalar(release).family == ScalarFamily::Release);
    }
  }
  { // Real production CP delivery token: idle wake, owned mailbox, teardown.
    struct Cp {
      int stores{};
      std::deque<std::function<void()>> messages;
    } cp;
    auto delivery = std::make_shared<CpCompletionTarget<Cp>>(&cp);
    auto post = [](Cp &target, auto message) {
      target.messages.emplace_back(std::move(message));
    };
    auto ready = [](Cp &target) { ++target.stores; };
    delivery->PostReady(post, ready, [](Cp &) {});
    assert(cp.stores == 0 && cp.messages.size() == 1);
    cp.messages.front()();
    cp.messages.pop_front();
    assert(cp.stores == 1);
    delivery->PostReady(post, ready, [](Cp &) {});
    delivery->Invalidate();
    cp.messages.front()();
    cp.messages.pop_front();
    assert(cp.stores == 1);
    delivery->PostReady(post, ready, [](Cp &) {});
    assert(cp.messages.empty());
  }
  { // Idle CP consumes the production delivery token without any later submit.
    struct Cp {
      std::mutex mutex;
      std::condition_variable_any cv;
      std::deque<std::function<void()>> messages;
      std::atomic<int> published{};
      GuestCompletionLane *lane{};
    } cp;
    SubmittedPrefix<int> prefix;
    auto ticket =
        *prefix.SubmitCurrent(1, [](auto &) {}, [](auto) { return 0; }, [] {});
    std::atomic<bool> complete{};
    auto delivery = std::make_shared<CpCompletionTarget<Cp>>(&cp);
    GuestCompletionLane lane(
        prefix.Identity(),
        [&](auto &, std::stop_token stop) {
          while (!stop.stop_requested()) {
            if (complete)
              return WaitStatus::Completed;
            std::this_thread::sleep_for(1ms);
          }
          return WaitStatus::Cancelled;
        },
        [&] {
          delivery->PostReady(
              [](Cp &target, auto message) {
                std::scoped_lock lock{target.mutex};
                target.messages.emplace_back(std::move(message));
                target.cv.notify_one();
              },
              [](Cp &target) {
                target.lane->Drain([&](auto &) { ++target.published; });
              },
              [](Cp &) {});
        });
    cp.lane = &lane;
    std::jthread processor([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        std::function<void()> message;
        {
          std::unique_lock lock{cp.mutex};
          cp.cv.wait(lock, stop, [&] { return !cp.messages.empty(); });
          if (stop.stop_requested())
            break;
          message = std::move(cp.messages.front());
          cp.messages.pop_front();
        }
        message();
      }
    });
    assert(lane.Enqueue(ticket, {}));
    assert(cp.published == 0);
    complete = true;
    for (int i = 0; i < 2000 && !cp.published; ++i)
      std::this_thread::sleep_for(1ms);
    assert(cp.published == 1);
    delivery->Invalidate();
    lane.Stop();
    processor.request_stop();
    cp.cv.notify_all();
    processor.join();
  }
  { // A prior Finish proof must not hide a later terminal waiter failure.
    SubmittedPrefix<int> prefix;
    auto t =
        *prefix.SubmitCurrent(1, [](auto &) {}, [](auto) { return 0; }, [] {});
    std::atomic<bool> started{}, fail{};
    std::atomic<int> wake{};
    GuestCompletionLane lane(
        prefix.Identity(),
        [&](auto &, std::stop_token stop) {
          started = true;
          while (!fail && !stop.stop_requested())
            std::this_thread::sleep_for(1ms);
          return WaitStatus::Failed;
        },
        [&] { ++wake; });
    assert(lane.Enqueue(t, {}));
    while (!started)
      std::this_thread::sleep_for(1ms);
    auto proof = *prefix.ObserveCompletion(t, [](auto) { return 0; });
    lane.CompleteThrough(proof);
    fail = true;
    for (int i = 0; i < 2000 && !wake; ++i)
      std::this_thread::sleep_for(1ms);
    assert(lane.Failed());
    lane.Drain([](auto &) { assert(false); });
    assert(!lane.Enqueue(t, {}));
  }
  std::cout << "production completion adapters: PASS\n";
}
