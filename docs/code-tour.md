# Code tour

A reading order for someone opening CppKV for the first time: about 3,400 lines of C++ in `src/`, comments
included. Each step builds on the previous one; each file's header comment says what it owns and what it does not.

## 1. The data: `src/cppkv/store/kv_store.{hpp,cpp}`

A thin wrapper around `std::unordered_map<std::string, std::string>` with *transparent* hashing, so a lookup by
`std::string_view` does not allocate. `get()` returns a copy on purpose: a reference into the map would dangle once
another thread changes it (measured to be cheap: `docs/benchmarks.md` O7). Not thread-safe by itself.

## 2. The protocol, without sockets: `src/cppkv/protocol/`

- `line_framer` — bytes in, complete lines out. Remembers how far it has scanned, so a line that arrives one byte
  at a time costs O(n), not O(n²); rejects a line as soon as it can no longer fit.
- `request_parser` — one line → `Request` or `ProtocolError` (a `std::variant`). The order of the checks is part of
  the specification (`docs/protocol.md` §5).
- `response_encoder` — `Response` → wire bytes; `VALUE <len> <bytes>` is length-prefixed.

## 3. One connection's logic: `src/cppkv/server/connection_handler.{hpp,cpp}`

The heart of the design. `feed(bytes)` then `process(out, limit)` runs framer → parser → dispatcher → encoder and
stops after ~64 KiB of replies (back-pressure). It never touches a socket, so `tests/unit/connection_handler_test.cpp`
can split a whole session at **every** byte position and check the replies are identical.

## 4. Executing commands: `src/cppkv/server/command_dispatcher.{hpp,cpp}`

One `std::mutex` around the store **and** the log: a mutation is appended to the log, then applied, then
acknowledged, all under the lock. Its comment explains why the lock lives here and not in `KvStore`, and why one
mutex was kept after measuring (ADR 0002, `docs/benchmarks.md` O8).

## 5. Durability: `src/cppkv/persistence/`

- `log_format` — the on-disk record: little-endian length, CRC-32, payload. Encoded byte by byte, never by copying
  structs, so the file is identical on every CPU.
- `append_log` — open + recover (torn tail vs. corruption, `strict`/`truncate`), append, sync policies, fail-stop
  after the first write or sync error. Read [ADR 0003](adr/0003-persistence.md) first; the code follows it section
  by section.
- `posix_file` — the few POSIX calls, with `EINTR` and short writes handled, and `F_FULLFSYNC` on macOS.

## 6. The network: `src/cppkv/server/client_session.*` and `tcp_server.*`

- `ClientSession` — owns one socket and one `ConnectionHandler`; moves bytes, nothing else. Lifetime by
  `shared_from_this()` in every pending handler. Read the I/O-timeout and lingering-close comments together with
  [ADR 0004](adr/0004-operations.md): they record two subtle Asio facts (a `cancel()` can miss a multi-step
  `async_write`; a timer wait that already expired cannot be cancelled).
- `TcpServer` — acceptor, connection limit (`SessionRegistry`), signals, graceful shutdown as a small state machine
  on its own strand, accept back-off.

## 7. Putting it together: `src/main.cpp`

Declaration order is destruction order: logger → store → log → dispatcher → server, so the server is destroyed
first and everything it refers to is still alive while it shuts down.

## Tests worth reading

| Test | Shows |
|------|-------|
| `ConnectionHandler.FullSessionSplitAtEveryPairOfPositions` | Framing is independent of how TCP splits the stream |
| `CommandDispatcherConcurrency.ParallelCommandsAreAtomic` | The lock; fails under TSan without it |
| `CrashTest.*` (`tests/integration/persistence_test.cpp`) | The real binary killed with SIGKILL mid-load, then restarted |
| `OperationsTest.GracefulShutdownAnswersReceivedRequestsThenCloses` | Shutdown answers exactly the requests that were executed |
| `OperationsTest.AcceptBacksOffWhenOutOfFileDescriptorsAndRecovers` | Real `EMFILE` via `RLIMIT_NOFILE` (and the Linux/macOS difference) |
| `LoadgenTest.OpenLoopSendsExactlyTheScheduledNumberOfRequests` | The load generator is itself tested |

## Where to look for…

| Question | File |
|----------|------|
| What exactly is a valid request? | `docs/protocol.md`, `src/cppkv/protocol/request_parser.cpp` |
| What does `OK` promise after a crash? | `docs/protocol.md` §8, ADR 0003 |
| How is a slow or malicious client contained? | `docs/protocol.md` §7, `client_session.cpp`, ADR 0004 |
| Why these threads and this lock? | ADR 0002, `docs/benchmarks.md` O3 and O8 |
| What is not tested, and why? | `docs/quality.md` §4 |
