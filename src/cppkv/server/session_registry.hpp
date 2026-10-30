#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace cppkv {

class ClientSession;

// Tracks the live client sessions of a server so that it can
// (1) enforce the connection limit and (2) close every session on stop().
//
// Ownership: the registry holds only weak_ptrs. A session's lifetime is
// governed by its pending asynchronous operations (see ClientSession); the
// registry never keeps a finished session alive.
//
// Thread safety: every member function may be called from any thread. A mutex
// guards the map and the closed flag; sessions start and finish on different
// I/O threads at the same time.
class SessionRegistry {
public:
    enum class AddResult {
        added,    // the session counts against the limit until remove()
        full,     // max_sessions are already registered
        closed,   // the server is stopping; no new sessions
    };

    explicit SessionRegistry(std::size_t max_sessions) : max_sessions_{max_sessions} {}

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    AddResult try_add(const std::shared_ptr<ClientSession>& session);

    // Releases the session's slot. Safe to call for a session that was never
    // added or was already removed.
    void remove(const ClientSession* session);

    // Refuses all future additions and returns every session that is still
    // alive, so the caller can ask each one to close. Idempotent.
    [[nodiscard]] std::vector<std::shared_ptr<ClientSession>> close_and_collect();

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] bool is_closed() const;

private:
    mutable std::mutex mutex_;
    const std::size_t max_sessions_;
    bool closed_ = false;
    // Keyed by address: a session removes itself before it is destroyed, so an
    // address cannot be reused by a new session while its old entry exists.
    std::unordered_map<const ClientSession*, std::weak_ptr<ClientSession>> sessions_;
};

} // namespace cppkv
