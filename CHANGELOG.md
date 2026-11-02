# Changelog

## 1.0.0 — 2026-11-02

The first complete version: the project's Definition of Done (`docs/architecture.md` §3) is met.

**Server**
- Protocol v1 ([`docs/protocol.md`](docs/protocol.md)): `PING`, `SET`, `GET`, `DEL`, `EXISTS`, `DBSIZE`, `QUIT` over
  LF-terminated text lines; length-prefixed `VALUE` replies; typed `ERR` codes; fragmented and pipelined input.
- Concurrency ([ADR 0002](docs/adr/0002-concurrency-model.md)): asynchronous Boost.Asio on a fixed thread pool, one
  strand per connection, one store mutex; connection limit with `ERR BUSY`.
- Durability ([ADR 0003](docs/adr/0003-persistence.md)): append-only log with length + CRC-32 records, written before
  the change is applied; `--sync always|interval|never` (`F_FULLFSYNC` on macOS); recovery that truncates a torn
  tail and refuses (or, with `--recovery truncate`, cuts) a corrupted log; fail-stop on write errors; one server
  per data directory.
- Operations ([ADR 0004](docs/adr/0004-operations.md)): graceful shutdown on SIGINT/SIGTERM with a timeout, lingering
  close, idle/stalled-connection timeout, accept back-off on descriptor exhaustion, `TCP_NODELAY`, bounded request
  and output sizes, documented exit status, resource figures in the log.

**Tooling**
- `cppkv-loadgen`: closed- and open-loop load generator with coordinated-omission-free latency percentiles.
- Google Benchmark micro-benchmarks; `scripts/bench_load.sh` (load matrix), `scripts/coverage.sh`, `scripts/demo.sh`.
- CMake presets for debug, release, ASan+UBSan, TSan, CI (`-Werror`) and benchmarks; GitHub Actions on Linux
  (GCC, Clang, sanitizers incl. LeakSanitizer) and macOS.

**Verification** ([`docs/quality.md`](docs/quality.md)): 234 tests (unit, real-TCP integration, crash and signal tests
against the real binary); sanitizer-clean on Linux and macOS; 93.7 % line coverage; mutation checks per phase;
measured performance with raw data ([`docs/benchmarks.md`](docs/benchmarks.md)).

### History by phase

| Phase | Commit message (abridged) |
|-------|---------------------------|
| 0 | Requirements and architecture (`docs/architecture.md`) |
| 1 | Project skeleton: CMake presets, warnings, sanitizers, GoogleTest |
| 2 | In-memory store |
| 3 | Synchronous TCP server ([ADR 0001](docs/adr/0001-synchronous-iterative-server.md)) |
| 4 | Protocol: framer, parser, dispatcher, encoder; all v1 commands over TCP |
| 5 | Concurrent clients on an async thread pool with a connection limit |
| 6 | Checksummed append-only log with crash recovery |
| 7 | Graceful shutdown, signals, timeouts, lingering close, accept back-off |
| 8 | Micro-benchmarks, load generator, measurements; fixed a Nagle stall and a per-request timer cost found by measuring |
| 9 | CI on Linux and macOS; quality audit, coverage, static analysis |
| 10 | Portfolio documentation, demo script, version 1.0.0 |
