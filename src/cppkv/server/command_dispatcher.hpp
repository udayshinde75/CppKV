#pragma once

#include "cppkv/persistence/append_log.hpp"
#include "cppkv/protocol/protocol.hpp"
#include "cppkv/store/kv_store.hpp"

#include <mutex>

namespace cppkv {

// Executes validated requests against the store. No parsing, no I/O.
//
// Ownership: holds non-owning references to the store and (optionally) the
// log; both must outlive the dispatcher (main declares them first).
//
// Persistence (Phase 6): with a log, every mutation is written to the log
// FIRST and applied to memory only if the write (and, with --sync always, the
// fsync) succeeded. Both happen under the same mutex, so the order of records
// in the log is exactly the order in which clients observed the changes. If
// the log has failed, mutations are refused with ERR INTERNAL; reads continue.
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
    // `log` may be nullptr: the store is then purely in-memory (--in-memory, tests).
    explicit CommandDispatcher(KvStore& store, AppendLog* log = nullptr) : store_{store}, log_{log} {}

    CommandDispatcher(const CommandDispatcher&) = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;

    // Taken by value: SET moves the key and value into the store, so a caller
    // that passes a temporary Request pays for no copies.
    [[nodiscard]] Response execute(Request request);

private:
    std::mutex mutex_;   // guards every access to store_ and every append to log_
    KvStore& store_;
    AppendLog* log_;
};

} // namespace cppkv
