#pragma once

#include "cppkv/protocol/protocol.hpp"
#include "cppkv/store/kv_store.hpp"

namespace cppkv {

// Executes validated requests against the store. No parsing, no I/O.
//
// Ownership: holds a non-owning reference to the store; the store must
// outlive the dispatcher (main declares the store first).
//
// Thread safety: none in Phase 4 — one session runs at a time. Phase 5 adds
// synchronisation when several sessions share the dispatcher.
class CommandDispatcher {
public:
    explicit CommandDispatcher(KvStore& store) : store_{store} {}

    CommandDispatcher(const CommandDispatcher&) = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;

    // Taken by value: SET moves the key and value into the store, so a caller
    // that passes a temporary Request pays for no copies.
    [[nodiscard]] Response execute(Request request);

private:
    KvStore& store_;
};

} // namespace cppkv
