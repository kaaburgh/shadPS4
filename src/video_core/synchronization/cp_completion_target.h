// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <memory>
#include <mutex>
namespace VideoCore::Sync {
// The owner stops admission, invalidates this token, stops its lane and joins the
// CP before destroying publication targets. A callback already executing on that
// CP is covered by the join; never hold this mutex across guest stores or IRQs.
template <class Owner>
class CpCompletionTarget : public std::enable_shared_from_this<CpCompletionTarget<Owner>> {
public:
    explicit CpCompletionTarget(Owner* owner) : owner{owner} {}
    void Invalidate() {
        std::scoped_lock lock{mutex};
        owner = nullptr;
    }
    template <class Post, class Ready, class Wake>
    void PostReady(Post post, Ready ready, Wake wake) {
        auto state = this->shared_from_this();
        std::scoped_lock lock{mutex};
        if (!owner)
            return;
        post(*owner, [state, ready] {
            Owner* target;
            {
                std::scoped_lock lock{state->mutex};
                target = state->owner;
            }
            if (target)
                ready(*target); // CP only; owner destruction waits for CP exit.
        });
        wake(*owner);
    }

private:
    std::mutex mutex;
    Owner* owner;
};
} // namespace VideoCore::Sync
