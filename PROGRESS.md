# CppKV — Progress

## Current phase

**Phase 4 — Protocol parser and command dispatcher (MVP): complete on Linux; awaiting macOS run and approval.**

## Completed

### Phase 0 — Requirements and architecture (2026-10-28/29)

- `docs/architecture.md`: requirements, MVP/non-goals, components, protocol v1 draft, store semantics,
  concurrency options, persistence draft, error handling, limits, layout, dependencies, platform concerns,
  acceptance criteria, risks. Approved 2026-10-29 (including sanitizer presets in Phase 1).

### Phase 1 — Project skeleton and build system (2026-10-29)

- CMake 3.25…4.4, C++20, `CppKV::core` static library + `cppkv-server` (`--version`, `--help`, exit 2 on usage error).
- Presets `debug`, `release`, `asan-ubsan`, `tsan`, `ci`; per-target warnings; directory-wide sanitizers.
- Boost (headers only, 1.83–1.90, `BOOST_ASIO_NO_DEPRECATED`); GoogleTest 1.17.0 via `FetchContent` + SHA-256.
- Verified on Linux (12 configurations) and on macOS (Apple clang 21: debug, asan-ubsan, tsan, ci).

### Phase 2 — In-memory key-value store (2026-10-29)

- `src/cppkv/store/kv_store.{hpp,cpp}`: `set` (insert-or-replace, returns inserted), `get` (owning copy or
  `nullopt`), `erase`, `contains`, `size`. Heterogeneous `string_view` lookup. Non-copyable, non-movable.
- Semantics, ownership, thread-safety (none) and exception-safety (strong for `set`) documented in the header.
- `tests/unit/kv_store_test.cpp`: 21 tests + 4 `static_assert`s — empty store, insert vs replace, empty value vs
  missing key, spaces, case sensitivity, binary bytes and embedded NUL, empty key, 1 MiB value, copy
  independence, sink-parameter behaviour, lookup via buffer view, erase semantics, size bookkeeping, 10'000-key rehash.
- Mutation check: replacing `insert_or_assign` with `try_emplace` (which does not overwrite) makes 2 tests fail.
- Verified on macOS (debug, asan-ubsan: 31/31; ci builds).

### Phase 3 — TCP server and client sessions (2026-10-30)

- `TcpServer` (`src/cppkv/server/tcp_server.*`): synchronous, iterative; binds/listens in the constructor
  (throws on failure), `run()` serves one client at a time, thread-safe `stop()` via atomic flag + wake-up connection.
- `ClientSession` (`client_session.*`): owns the socket; `read_until` on a size-limited `dynamic_buffer` frames
  '\n'-terminated lines (fragmented and pipelined input), strips an optional CR, enforces 64 KiB line limit
  (`ERR TOO_LARGE`, then close). Phase 3 commands: `PING`, `QUIT`; others → `ERR UNKNOWN_COMMAND` (sanitised echo).
  Disconnects, mid-line disconnects, resets and write failures are logged, never thrown.
- `Logger` (`common/logger.*`): levelled, UTC-timestamped, mutex-protected lines to any `std::ostream`.
- `parse_command_line` (`common/config.*`): `--bind`, `--port` (strict `from_chars`), `--version`, `--help`;
  errors as values. `main` reports "cannot listen on …" and exits 1 when the port is in use.
- Tests: unit — 9 config, 4 logger (incl. 4-thread concurrency); integration — 25 tests over real TCP
  (`tests/integration/`), client reads have a 5 s deadline so a missing reply fails fast; CLI `cli.invalid_port`.
- ADR 0001 records the synchronous model and its limitations.
- Verified on macOS (debug, asan-ubsan, tsan: 70/70, 1 test skipped by design; ci builds).

### Phase 4 — Protocol parser and command dispatcher (2026-10-30)

- `docs/protocol.md`: normative protocol v1 — framing, syntax, key/value rules, responses, error codes and the
  order in which they are checked, limits, examples.
- Pure protocol layer (`src/cppkv/protocol/`): `protocol.hpp` (Request, ProtocolError, Response, ErrorCode),
  `LineFramer` (single-scan framing, early TOO_LARGE, views valid until next feed), `parse_request`
  (variant result, ASCII-only case folding, unsigned byte checks), `encode_response` (appends; `VALUE <len> <bytes>`).
- `CommandDispatcher`: executes requests on `KvStore`; Request taken by value so SET moves strings into the store.
- `ConnectionHandler` (sans-I/O): framer → parser → dispatcher → encoder; output in ~64 KiB batches; nothing
  executed after QUIT / TOO_LARGE.
- `ClientSession` reduced to I/O: `read_some` 4 KiB → `feed`; `process` → one `write` per batch; catches
  exceptions → `ERR INTERNAL` + close. `main` owns `KvStore` and `CommandDispatcher`, passed to `TcpServer`.
- Tests: framer 18, parser 15, encoder 6, dispatcher 8, handler 8 (incl. every 3-way split of a 14-line
  session = 7'381 conversations), 9 new TCP tests (all commands, byte-by-byte SET, NUL bytes, 60 KB value,
  ~6 MB pipelined output, data shared across connections, store unchanged after QUIT).
- Mutation checks (each caught): framer CR-at-limit rule, ignoring `closes_connection`, DEL via `contains`,
  SET value trimming, removing the output limit.

## Environment

- **Linux sandbox:** Ubuntu 24.04.5 x86_64; GCC 13.3.0, Clang 18.1.3; CMake 3.28.3 + 4.4.0; Boost 1.83.0 + 1.90.0.
  Clang sanitizer runtimes unavailable (cannot be installed).
- **macOS (MacBook Air M2):** macOS 27.0.1, Apple clang 21.0.0, CMake 4.4.0, Ninja 1.13.2, Boost 1.90.0 (Homebrew),
  Command Line Tools only, `ulimit -n` 2560.

## Design decisions

- Pure, socket-free framer / parser / dispatcher / encoder / store; only session and server touch Asio.
- Protocol v1 draft: LF lines (optional CR stripped), single-space tokens, `SET` value = rest of line,
  `VALUE <len> <bytes>`, `ERR <CODE> <message>`.
- Modern Asio API only, enforced by `BOOST_ASIO_NO_DEPRECATED`; `-Wnull-dereference` Clang-only.
- Store accepts any bytes; key/value rules are enforced by the parser (Phase 4).
- `get` returns a copy (safe once concurrency arrives); performance impact to be measured in Phase 8, not assumed.
- Store method names follow the standard library (`erase`, `contains`); the dispatcher maps `DEL`/`EXISTS` to them.
- Phase 3 server is synchronous and iterative (ADR 0001); concurrency model decided in Phase 5.
- No process-wide SIGPIPE handler: Asio suppresses SIGPIPE per socket (verified in its sources, covered by a test).
- Tests synchronise only on observable events; timeouts exist solely as failure detectors (5 s client deadline,
  30 s CTest TIMEOUT on integration tests).
- Sans-I/O `ConnectionHandler`: protocol behaviour is tested without sockets; Phase 5 can change the I/O model alone.
- `Request` owns its strings: same copy count as views for SET (the store needs owned strings anyway), and no
  lifetime coupling to the receive buffer.
- Framer returns `string_view` (valid until next `feed`) to avoid a copy per line; the parser copies what it keeps.

## Build commands

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan && ctest --preset asan-ubsan
ctest --preset debug -L integration          # TCP tests only
./build/debug/src/cppkv-server --port 7379   # then: nc 127.0.0.1 7379, try SET/GET
```

## Test results

| Date | Platform | Configurations | Result |
|------|----------|----------------|--------|
| 2026-10-29 | Linux | GCC 13.3 / Clang 18.1 × CMake 3.28 / 4.4 × Boost 1.83 / 1.90; debug, release, ci, asan-ubsan (GCC), tsan (GCC) — 12 total | Phase 1: 10/10; Phase 2: 31/31 in every configuration; 0 warnings from CppKV code |
| 2026-10-29 | macOS | Apple clang 21, CMake 4.4, Boost 1.90; debug, asan-ubsan, tsan, ci | Phase 1: 10/10, no sanitizer reports, `ci` builds with `-Werror` |
| 2026-10-29 | macOS | Phase 2: debug, asan-ubsan, ci | 31/31, no sanitizer reports, `ci` builds |
| 2026-10-30 | Linux | Same 12 configurations | Phase 3: 70/70 in every configuration; 0 warnings from CppKV code. Integration suite repeated 100× under ASan+UBSan, TSan and release (Clang): 0 failures, 0 sanitizer reports |
| 2026-10-30 | macOS | Phase 3: debug, asan-ubsan, tsan, ci | 70/70 each (wildcard-bind test skipped on macOS by design), no sanitizer reports |
| 2026-10-30 | Linux | Same 12 configurations | Phase 4: 134/134 in every configuration; 0 warnings from CppKV code. Integration suite (34 tests) repeated 100× under ASan+UBSan, TSan and Clang release: 0 failures, 0 sanitizer reports |
| — | macOS | Phase 4 | **not yet run** |

## Known issues

- GCC `tsan` builds print `-Wtsan` (`atomic_thread_fence` unsupported) from Asio's `std_fenced_block`; TSan does
  not model standalone fences. Investigate in Phase 5 before relying on GCC TSan.
- Clang sanitizer builds are untested on Linux (sandbox limitation); covered on macOS.
- Strong exception guarantee of `KvStore::set` is argued from the standard, not tested: testing it would need
  allocation-failure injection (a custom allocator), which is not worth the complexity yet.
- Unseeded string hash → hash-flooding risk if exposed to untrusted clients (documented in architecture §6).
- Phase 3 server limitations (ADR 0001): one client at a time; `stop()` waits for the active client; no idle
  timeout; closing with unread data sends RST and may lose the final reply (lingering close planned for Phase 7);
  persistent `accept()` errors would busy-loop.
- Ctrl-C terminates the process immediately (exit 130); graceful shutdown is Phase 7.

## Next phase

**Phase 5 — Concurrent clients:** choose and justify a concurrency model (ADR 0002) against the Phase 4
single-client baseline; synchronise shared state; bounded connections/threads; concurrent read/write tests;
TSan on Linux and macOS (including the GCC `-Wtsan` fence question).
