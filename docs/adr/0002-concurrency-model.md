# ADR 0002 — Concurrency model: async Asio on a fixed thread pool

- **Status:** accepted (Phase 5); supersedes the server model of [ADR 0001](0001-synchronous-iterative-server.md)
- **Date:** 2026-10-30

## Context

The Phase 4 server serves one client at a time. One idle client blocks everyone, and `stop()` cannot
interrupt a connected client. Phase 5 must let many clients use the store safely, with bounded threads and
connections, and validate the result with ThreadSanitizer.

The protocol logic is already separated from I/O (`ConnectionHandler`, sans-I/O), so only the session and the
server change. The store is the only state shared between clients.

## Options

| Model | Threads | Idle / slow client | Shutdown & timeouts | Shared-state locking | Complexity |
|-------|---------|--------------------|---------------------|----------------------|-----------|
| **A. Thread per connection** (capped) | = clients (≤ cap) | Costs a thread (stack, scheduler) | Hard: a blocking read can only be interrupted from another thread at the OS level | Needed | Low: Phase 4 session code stays as is |
| **B. Fixed pool + blocking sockets** | fixed | Blocks a pool thread; enough idle clients starve everyone | Hard (as A) | Needed | Medium: needs a queue of ready connections |
| **C. Async, 1 event-loop thread** | 1 | Free: costs only memory | Easy: cancel with `close()`; timers later | None (one thread) | Medium: callback lifetimes |
| **D. Async, N event-loop threads, strand per session** | fixed N | Free | Easy (as C) | Needed for shared state only | Medium-high |

Notes:
- A is the classic first design. It fails "no unbounded thread creation" unless capped, and even capped, 128 idle
  clients mean 128 sleeping threads.
- B looks bounded, but it is a trap with blocking I/O: N idle clients occupy all N threads and nobody else
  gets served — the Phase 3 problem multiplied.
- C is attractive because it needs no locks at all. But one slow step (a large value copy now, an `fsync` in
  Phase 6) stalls every client, and it uses one core.
- D keeps C's properties (idle clients are free, cancellation is easy) and uses several cores.

## Decision

**D: asynchronous Boost.Asio, one `io_context` run by a fixed pool of threads (`--threads`, default 4), one strand
per session, one `std::mutex` around the store, and a hard connection limit (`--max-connections`, default 128).**

How it fits together:

- `TcpServer::run()` runs `io_context::run()` on the calling thread plus `threads − 1` `std::jthread`s.
- Each accepted socket gets a new **strand** as its executor. Asio runs one session's handlers one at a time,
  so session members need no locks. Different sessions run in parallel.
- A session's lifetime is owned by its pending operations: each completion handler holds
  `shared_from_this()`. When the last handler returns without starting a new operation, the session is destroyed.
- `SessionRegistry` (mutex-protected, weak references only) enforces the connection limit and lets `stop()`
  close every session. A further client gets `ERR BUSY too many connections` and is disconnected.
- `CommandDispatcher` holds the store mutex for the duration of one command's in-memory operation only.
  Parsing, response encoding and socket I/O happen outside the lock.
- The acceptor also has its own strand, so `stop()`'s `close()` and accept completions never race.

## The data race this prevents

Two clients on two threads send `SET a 1` and `SET b 2` at the same moment. Both threads call
`std::unordered_map::insert_or_assign`. If the first insert pushes the load factor over the limit, the map
rehashes: it allocates a new bucket array and relinks every node. If the second thread is walking the old buckets
at that moment, it follows pointers that are being rewritten. The result is undefined behaviour — a lost key, a
corrupted map, or a crash, and often only under load.

The mutex in `CommandDispatcher::execute` makes each command atomic with respect to every other.
**Verified:** with the lock removed, ThreadSanitizer reports 90 data races inside `std::_Hashtable` in
`CommandDispatcherConcurrency.ParallelCommandsAreAtomic`, and the integration test
`ManyConcurrentClientsReadTheirOwnWrites` fails under TSan too, with the race located in
`_Hashtable::_M_insert_unique_node`, the insert step that triggers a rehash. With the lock, both are clean.

## Why the lock is in the dispatcher, and why only one

- Phase 6 must append to the log **and** update memory as one step, so that the log order equals the order in
  which clients observed changes. A lock inside `KvStore` cannot cover the log write.
- One `std::mutex` is the simplest correct choice. `std::shared_mutex` (parallel readers) or lock striping
  (several shards) might reduce contention, but they cost something too. Phase 8 measures before changing it.

## Consequences

Resolved from ADR 0001:
- Idle or slow clients no longer block others (test `IdleClientDoesNotBlockOthers`).
- `stop()` closes connected clients immediately (test `StopClosesConnectedClients`).
- Threads are bounded by configuration, never by the number of clients.

Still open (Phase 7 unless noted):
- **No idle timeout:** an idle client holds one of the connection slots indefinitely.
- **`stop()` is abrupt:** in-flight responses may be cut off. Graceful draining is Phase 7.
- **Closing with unread input sends RST** (ADR 0001); this also applies to `ERR BUSY` if the rejected client has
  already sent data. A lingering close is Phase 7.
- **Persistent `accept()` errors** (e.g. `EMFILE`) are retried immediately, which could spin.
- **The store lock serialises all commands.** That's fine at this scale; Phase 8 measures contention.

## Validation

- Unit: `SessionRegistryTest.ConcurrentAddsNeverExceedTheLimit` (8 threads), `CommandDispatcherConcurrency.*`.
- Integration (server on 4 threads): 16 concurrent clients reading their own writes, 8 concurrent pipelined
  writers to the same keys, connection limit with deterministic slot reuse, stop with connected clients.
- ThreadSanitizer on Linux with GCC 13: no reports in the full suite, nor in the integration suite repeated 30×.
  GCC prints `-Wtsan: atomic_thread_fence is not supported`: TSan does not model the fence Asio uses when it
  invokes handlers. Such a gap could cause *false* reports; none were observed. Clang's TSan on macOS is the
  cross-check. Disabling Asio's fences (`BOOST_ASIO_DISABLE_FENCED_BLOCK`) was rejected, because it would
  change what is being tested.
- No performance claims are made here. Throughput and latency of this model are measured in Phase 8.
