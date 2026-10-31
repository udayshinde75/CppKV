#include "cppkv/server/session_registry.hpp"

namespace cppkv {

SessionRegistry::AddResult SessionRegistry::try_add(const std::shared_ptr<ClientSession>& session) {
    const std::lock_guard lock{mutex_};
    // Check and insert under the same lock: otherwise two threads could both
    // see "127 of 128 used" and both insert, exceeding the limit.
    if (closed_) {
        return AddResult::closed;
    }
    if (sessions_.size() >= max_sessions_) {
        return AddResult::full;
    }
    sessions_.emplace(session.get(), session);
    return AddResult::added;
}

void SessionRegistry::remove(const ClientSession* session) {
    std::function<void()> notify;
    {
        const std::lock_guard lock{mutex_};
        sessions_.erase(session);
        if (closed_ && sessions_.empty()) {
            notify = std::move(on_drained_);
            on_drained_ = nullptr;
        }
    }
    // Called outside the lock: the callback may take other locks or post work,
    // and must never be able to deadlock against a concurrent remove().
    if (notify) {
        notify();
    }
}

std::vector<std::shared_ptr<ClientSession>> SessionRegistry::close_and_collect(std::function<void()> on_drained) {
    std::vector<std::shared_ptr<ClientSession>> alive;
    std::function<void()> notify;
    {
        const std::lock_guard lock{mutex_};
        // Setting closed_ and collecting under one lock means a session is either
        // collected here or rejected by try_add() — never neither.
        closed_ = true;
        alive.reserve(sessions_.size());
        for (const auto& [address, weak] : sessions_) {
            if (std::shared_ptr<ClientSession> session = weak.lock()) {
                alive.push_back(std::move(session));
            }
        }
        if (on_drained) {
            if (sessions_.empty()) {
                notify = std::move(on_drained);
            } else {
                on_drained_ = std::move(on_drained);
            }
        }
    }
    if (notify) {
        notify();
    }
    return alive;
}

std::size_t SessionRegistry::size() const {
    const std::lock_guard lock{mutex_};
    return sessions_.size();
}

bool SessionRegistry::is_closed() const {
    const std::lock_guard lock{mutex_};
    return closed_;
}

} // namespace cppkv
