#include "cppkv/server/command_dispatcher.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace cppkv {

namespace {

Response boolean(bool value) {
    return Response::integer_value(value ? 1U : 0U);
}

Response not_persisted(const AppendLog& log) {
    return Response::failure(ErrorCode::internal, "write not persisted (" + log.failure_reason() + ")");
}

} // namespace

Response CommandDispatcher::execute(Request request) {
    // Commands that do not touch the store need no lock.
    if (request.command == CommandType::ping) {
        return Response::pong();
    }
    if (request.command == CommandType::quit) {
        return Response::bye();
    }

    // Without this lock, two sessions on different threads could run
    // store_.set() at the same time. If one insert makes the hash table grow
    // (rehash), the other thread may follow bucket pointers that are being
    // rewritten: a data race, i.e. undefined behaviour — lost keys or a crash.
    // ThreadSanitizer reports exactly this if the lock is removed.
    //
    // The lock covers the log append and the in-memory operation. Parsing
    // happened before, and encoding and socket I/O happen after, without it.
    // Cost: with --sync always, the fsync runs under the lock, so every other
    // command waits for it — durability is paid for in latency of all clients.
    // Batching several clients' records into one fsync ("group commit") would
    // reduce that; it is a Phase 8 candidate, to be justified by measurements.
    const std::lock_guard lock{mutex_};

    switch (request.command) {
    case CommandType::set:
        // Log first: if the server crashes after this line, recovery replays
        // the SET even though the client never got OK — harmless. The opposite
        // order could send OK for a change that is lost in a crash.
        if (log_ != nullptr && !log_->append_set(request.key, request.value)) {
            return not_persisted(*log_);
        }
        store_.set(std::move(request.key), std::move(request.value));
        return Response::ok();
    case CommandType::get: {
        // get() copies the value while the lock is held; the Response then owns
        // its bytes, so no reference into the map escapes the critical section.
        std::optional<std::string> value = store_.get(request.key);
        return value ? Response::value(std::move(*value)) : Response::nil();
    }
    case CommandType::del:
        if (!store_.contains(request.key)) {
            return boolean(false);   // nothing changes, so nothing is logged
        }
        if (log_ != nullptr && !log_->append_del(request.key)) {
            return not_persisted(*log_);
        }
        return boolean(store_.erase(request.key));
    case CommandType::exists:
        return boolean(store_.contains(request.key));
    case CommandType::dbsize:
        // size_t is uint64_t on 64-bit Linux, a distinct type of the same
        // width on macOS. The implicit conversion is lossless on every
        // supported platform; an explicit cast would be "useless" on Linux.
        static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
        return Response::integer_value(store_.size());
    case CommandType::ping:
    case CommandType::quit:
        break;   // handled above, without the lock
    }
    return Response::failure(ErrorCode::internal, "unhandled command");   // unreachable
}

} // namespace cppkv
