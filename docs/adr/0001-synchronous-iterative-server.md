# ADR 0001 — Synchronous, iterative TCP server for Phase 3

- **Status:** superseded by [ADR 0002](0002-concurrency-model.md) (Phase 5). Kept as the record of the Phase 3–4 design;
  the limitations below still apply where ADR 0002 says so (RST on close with unread data, no idle timeout).
- **Date:** 2026-10-30

## Context

Phase 3 needs a runnable TCP server that handles `PING`, `QUIT`, disconnects and network errors.
Concurrency is deliberately scheduled for Phase 5, where the model is chosen after measuring a baseline.

## Decision

Use blocking Boost.Asio calls on one thread. `TcpServer::run()` accepts a client, runs a
`ClientSession` for it until it leaves, then accepts the next client.

- A session is a local object in the accept loop, so its lifetime is exactly one client, and RAII closes the socket.
- `read_until` on a `dynamic_buffer` with a maximum size does the framing and enforces the request-size limit.
- `stop()` is thread-safe: it sets an atomic flag and opens its own short connection to the server, so a thread
  blocked in `accept()` wakes up and sees the flag. It never touches the acceptor from another thread
  (Asio objects are not safe to share between threads).

## Consequences

Good:
- Simple control flow that reads top to bottom; no handlers, no shared state, no locks.
- The store (Phase 4) can be used without synchronisation while there is only one session at a time.

Accepted limitations (each is tested or documented):
- **One client at a time.** Further clients connect (the kernel completes the handshake and queues them in the
  listen backlog) but get no replies until the current client leaves. One idle client blocks everyone.
  Test: `QueuedClientIsServedAfterPreviousClientLeaves`.
- **`stop()` does not interrupt an active session**; `run()` returns after the current client leaves.
- **No idle timeout.** A client that connects and sends nothing, or sends half a line, holds the server forever.
- **Closing with unread data sends RST.** If the server closes a socket while the kernel still holds bytes the
  client sent (for example, more bytes after an oversized line, or requests after `QUIT` that arrived in a later
  segment), the kernel sends RST instead of FIN. Depending on timing and OS, the client may then see
  "connection reset" and can lose the final reply (`BYE` or `ERR TOO_LARGE`). The fix is a *lingering close*
  (shut down sending, then read and discard for a bounded time before closing), planned for Phase 7.
- **Persistent `accept()` errors** (e.g. `EMFILE` when out of file descriptors) are logged and retried
  immediately, which would busy-loop. Unlikely with one session at a time; resource limits are Phase 7.

## Alternatives considered

- **Thread per connection now:** would need synchronisation of the store in Phase 3 already, mixing two
  learning steps and pre-empting the Phase 5 decision.
- **Async Asio now:** removes all the limitations above, but callback-based lifetimes (`shared_from_this`)
  are harder to learn at the same time as framing and error handling. Phase 5 compares the models with
  this phase as the baseline.
