#pragma once

#include "cppkv/protocol/protocol.hpp"
#include "cppkv/store/kv_store.hpp"

#include <mutex>

namespace cppkv {

// Executes validated requests against the store. No parsing, no I/O.
//
// Ownership: holds a non-owning reference to the store; the store must
// outlive the dispatcher (main declares the store first).
//
// Thread safety: execute() may be called from any number of threads at once.
// Every store access happens under one mutex, so each command is atomic with
// respect to every other command.
//
// Why the lock lives here and not inside KvStore:
// - Phase 6 must append a mutation to the log AND apply it to memory as one
//   step, so that the log order equals the order in which clients observed the
//   changes. A lock inside KvStore could not cover the log write.
// - KvStore stays a simple, single-threaded container that is easy to test.
//
// Why one std::mutex and not std::shared_mutex or several shards:
// - It is the simplest correct choice. A reader-writer lock or sharding may
//   help under heavy contention, but they also cost something, and Phase 8
//   measures before anything is optimised.
class CommandDispatcher {
public:
    explicit CommandDispatcher(KvStore& store) : store_{store} {}

    CommandDispatcher(const CommandDispatcher&) = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;

    // Taken by value: SET moves the key and value into the store, so a caller
    // that passes a temporary Request pays for no copies.
    [[nodiscard]] Response execute(Request request);

private:
    std::mutex mutex_;   // guards every access to store_
    KvStore& store_;
};

} // namespace cppkv
