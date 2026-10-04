#include "video_core/synchronization/submitted_prefix.h"
#include <cassert>
#include <type_traits>
int main() {
  using Prefix = VideoCore::Sync::SubmittedPrefix<int>;
  static_assert(
      !std::is_default_constructible_v<VideoCore::Sync::SubmittedTick>);
  Prefix prefix;
  prefix.Sessions().push_back({1, 11, 12}); // Upload A before primary A.
  prefix.MarkAccessed();
  auto a = prefix.SubmitCurrent(
      1, [](const auto &) {},
      [&](auto commands) {
        assert((std::vector<int>(commands.begin(), commands.end()) ==
                std::vector<int>{11, 12}));
        return 0;
      },
      [&] { prefix.Sessions().push_back({2, 0, 22}); });
  assert(a && a->Value() == 1 && a->BelongsTo(prefix.Identity()));
  assert(prefix.Sessions().back().primary == 22); // B remains unsubmitted.
  Prefix other;
  assert(!a->BelongsTo(other.Identity()));
  auto failure = prefix.SubmitCurrent(
      2, [](const auto &) {}, [](auto) { return -4; }, [] { assert(false); });
  assert(!failure && failure.error() == -4);
  Prefix empty;
  empty.Sessions().push_back({3, 0, 32});
  assert(empty.KnownEmpty());
  auto ticket = empty.SubmitCurrent(
      1, [](const auto &) {}, [](auto) { return 0; }, [] {});
  assert(ticket);
}
