# CppKV — Progress

## Current phase

**Phase 9 — CI and quality: complete locally; the workflow runs on GitHub with the next push (by the owner).**

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
- Verified on macOS (debug, asan-ubsan, tsan: 134/134; ci builds).

### Phase 5 — Concurrent clients (2026-10-30)

- ADR 0002: compared thread-per-connection, pool + blocking I/O, single-thread async, multi-thread async; chose
  async Asio with one `io_context` on a fixed pool (`--threads`, default 4), a strand per session, one
  `std::mutex` in `CommandDispatcher`, and `--max-connections` (default 128) → `ERR BUSY`.
- `ClientSession` rewritten asynchronously: lifetime owned by pending handlers (`shared_from_this`), all
  handlers on its strand, `request_close()` posts to the strand; exceptions never reach the event loop.
- `SessionRegistry` (new): mutex-protected weak references; check-and-insert under one lock; slot released before
  the socket closes (deterministic reuse); `close_and_collect()` for stop.
- `TcpServer`: acceptor on its own strand; `run()` on N threads with exception-safe event loops; `stop()` closes
  the listener and every session (no more wake-up connection; connected clients no longer block stop).
- Tests: registry 5 (incl. 8-thread limit race), dispatcher 8-thread atomicity, config 2; integration 5 new
  (idle client doesn't block, 16 concurrent clients, 8 concurrent pipelined writers on shared keys, BUSY + slot
  reuse, stop with connected clients); Phase 3 queued-client test replaced.
- Data-race demonstration: removing the store lock → TSan reports 90 races in `std::_Hashtable` (unit) and fails
  the 16-client integration test; restored → clean.
- Verified on macOS (debug, asan-ubsan, tsan with Clang TSan: 146/146; ci builds).

### Phase 6 — Persistence and recovery (2026-10-30)

- ADR 0003: append-only log `<data-dir>/cppkv.log`; length + CRC-32 framed little-endian records; write path
  "log → sync per policy → apply to memory → reply" under the dispatcher mutex; durability table per policy;
  torn-tail vs corruption classification; fail-stop; flock.
- `persistence/log_format` (pure codec), `persistence/posix_file` (RAII fd, write_all, pread, `fdatasync` /
  `F_FULLFSYNC`, directory fsync, ftruncate), `persistence/append_log` (open + replay + repair, append with
  rollback, interval sync thread, sync counter).
- `CommandDispatcher` logs before applying; DEL of a missing key is not logged; failed log → `ERR INTERNAL write
  not persisted (...)`, reads continue.
- CLI: `--data-dir`, `--sync always|interval|never`, `--recovery strict|truncate`, `--in-memory`; `main` recovers
  before listening and exits 1 on corruption or a locked log.
- Tests: log format 9 (golden bytes from Python zlib), append log 18 (every cut point, zero tail, damaged last
  record, mid-file corruption strict/truncate, absurd length, foreign file, version, partial header, flock,
  sync counts, RLIMIT_FSIZE write failure), integration 7 (restart, 800 concurrent writes, real SIGKILL with
  sync always and never, torn tail, corrupted log exit status 1 + truncate override, second server refused).
- Mutation checks (each caught): apply-before-log, DEL not logged, no partial-record rollback, strict mode
  truncating, zero-tail rule removed, no fail-stop, no sync in `always`.
- `strace`: `--sync always` → exactly one `fdatasync` per SET; `interval`/`never` → none during a 20-SET burst.
- Verified on macOS (debug, asan-ubsan, tsan: 180/180 incl. all 34 persistence/crash tests; ci builds).

### Phase 7 — Reliability and operational behaviour (2026-10-31)

- ADR 0004. SIGINT/SIGTERM via `asio::signal_set` → graceful shutdown (stop accepting, answer requests received
  in full, lingering close), `--shutdown-timeout` (default 10 s) then forced close; a second signal stops
  immediately; exit 0. Shutdown state on the acceptor's strand; `SessionRegistry` drained callback ends it promptly.
- Lingering close (FIN, discard input ≤ 2 s / 64 KiB, close) for QUIT, TOO_LARGE, BUSY, shutdown.
- Per-session I/O timeout (`--idle-timeout`, default 300 s, 0 = off) for idle clients, half requests and stalled
  readers; implemented by closing, not `cancel()` (a cancel can miss a multi-step async_write — found by mutation
  testing); stale-timer check via `expiry()`.
- Accept back-off (100 ms) on EMFILE/ENFILE/ENOBUFS/ENOMEM; SIGPIPE ignored process-wide.
- `--max-line-bytes`, `--log-level`; startup line with all limits; per-connection `requests=` / `duration_ms=`;
  shutdown counters; `--help` documents signals and exit status. Exit 1 if the log failed while running.
- Tests: config 4 (incl. the Phase 6 persistence flags, previously untested), `OperationsTest` 9 (drain invariant,
  idle clients don't delay shutdown, shutdown timeout, BYE / TOO_LARGE + FIN while the client keeps sending, idle
  timeout + slot reuse, stalled reader, active client not closed, EMFILE back-off + recovery via RLIMIT_NOFILE),
  `ProcessTest` 6 (SIGTERM exit 0 + data kept, SIGINT, second signal, closed stderr pipe, port in use → 1,
  unusable data dir → 1).
- Mutation checks (caught): bare close, no back-off, SIGPIPE not ignored, drain without answering buffered
  requests, no I/O timeout, timeout via `cancel()`. Not observable on Linux: lingering vs FIN-first (measured and
  documented in ADR 0004).

### Phase 8 — Performance benchmarking (2026-10-31)

- `bench/`: Google Benchmark 1.10.0 (FetchContent, pinned by commit hash) — store, framer/parser/encoder, CRC-32,
  record encoding, `AppendLog` per sync policy, whole sans-I/O request path, dispatcher mutex under 1–8 threads.
  `bench.smoke` CTest runs every benchmark once (ci preset), so they cannot rot or crash unnoticed.
- `tools/loadgen` (`cppkv-loadgen`): closed loop with pipelining, open loop at a fixed rate measured from the
  scheduled send time (coordinated omission), separate client send-lag report, reply validation, log-linear
  latency histogram (≤ 0.79 % error), text/CSV output. `scripts/bench_load.sh`: 34-run matrix (clients, sync
  policy × read mix, I/O threads, pipelining, open-loop load levels, value size), fresh server per run,
  environment record.
- Server: logs open-file limit at startup, and `peak_rss_kib`, `cpu_user_ms`, `cpu_system_ms` at shutdown.
- Found by measurement and fixed: **missing `TCP_NODELAY`** (Nagle held replies until the client's next request;
  open-loop p95 0.89 → 0.14 ms at 4 × 1250 req/s); **I/O timer re-armed per operation** (2 `timerfd_settime` per
  request; −30 % throughput with 1 client on 4 threads) → armed once, re-arms itself; 3 mutations caught.
- Observations recorded with evidence in `docs/benchmarks.md`: work is ~75–85 % kernel time at depth 1 (pipelining
  ×15); `--sync always` serialises SETs at ~1/fsync and stalls reads behind the lock; CRC-32 = 67.6 % of user
  instructions for 16 KiB SETs (callgrind), inside the lock; thread hand-offs (~14 futex/request); GET copy and
  the single mutex are not bottlenecks. M2 run: `F_FULLFSYNC` 2.7 ms → 511 SET/s with `--sync always`; the log's
  `write(2)` costs 2.5 µs on APFS (SET-only `--sync never` −63 % vs in memory); `--threads 1` is the most efficient
  even on 8 cores (8 threads: 3.3× CPU per request). Candidates listed, not implemented: group commit (largest
  effect), record encoding outside the lock, faster CRC-32, io_context per thread.
- Tests: `LatencyHistogram` 8, `LoadOptions` 5, `ProcessStats` 3, `LoadgenTest` 6 (counts match server state,
  exact open-loop request count, misses, server stop and connection limit reported as failures).

### Phase 9 — CI and quality (2026-11-02)

- `.github/workflows/ci.yml`: Linux (ubuntu-24.04, Boost 1.83 from apt) GCC `ci`, Clang `ci`, Clang `release`,
  GCC ASan+UBSan+LSan, GCC TSan on every push/PR; macOS (macos-26, Homebrew Boost) AppleClang `ci` on pushes to
  main; macOS sanitizers on demand. Actions pinned by commit hash, read-only token, docs-only changes skipped,
  superseded runs cancelled, test log uploaded on failure. Validated with actionlint 1.7.12 (checksum-verified).
- Quality audit (`docs/quality.md`): LeakSanitizer shown to fire with the preset's options; coverage via
  `scripts/coverage.sh` (gcov JSON + `coverage_summary.py`); clang-tidy 18 pass — 3 kinds of finding fixed
  (int-multiplication widening ×7, no-op move ×2, missing nodiscard), the rest triaged and documented.
- Coverage: 90.8 % → **93.7 % of lines**, 96.6 % → **99.2 % of functions** (src/ + tools/); the rest is mostly
  fault-injection-only error handling, listed in docs/quality.md.
- Coverage gap found and closed: the load generator's CSV (read by `bench_load.sh` by column number) and its
  error counting were untested → `LoadReportFormat` 6, `LoadgenFakeServer` 3, 2 CLI tests, enumerated config
  values; 3 mutations of them caught.

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
- `get` returns a copy (safe once concurrency arrives); Phase 8 measured +21 ns (16 B) to +7.9 µs (64 KiB) per
  GET against 10–40 µs of server CPU per request: kept.
- Store method names follow the standard library (`erase`, `contains`); the dispatcher maps `DEL`/`EXISTS` to them.
- Phase 3 server is synchronous and iterative (ADR 0001); concurrency model decided in Phase 5.
- No process-wide SIGPIPE handler: Asio suppresses SIGPIPE per socket (verified in its sources, covered by a test).
- Tests synchronise only on observable events; timeouts exist solely as failure detectors (5 s client deadline,
  30 s CTest TIMEOUT on integration tests).
- Sans-I/O `ConnectionHandler`: protocol behaviour is tested without sockets; Phase 5 can change the I/O model alone.
- `Request` owns its strings: same copy count as views for SET (the store needs owned strings anyway), and no
  lifetime coupling to the receive buffer.
- Framer returns `string_view` (valid until next `feed`) to avoid a copy per line; the parser copies what it keeps.
- Concurrency (ADR 0002): async on a fixed thread pool; strand per session; one store mutex held only for the
  in-memory operation; thread count independent of client count; hard connection cap.
- Operations (ADR 0004): draining never cancels writes (they carry replies to executed requests); timeouts and
  forced stops close the socket instead of cancelling; lingering close bounded to 2 s / 64 KiB; slot released
  before FIN.
- Persistence (ADR 0003): log-before-apply under the same lock as the memory update; default `--sync always`;
  strict recovery by default (damage followed by data refuses startup); fail-stop after any write/sync failure
  (no fsync retries); one server per data directory via flock.
- Benchmarks (Phase 8): numbers are compared only within one machine; every optimisation needs a before/after
  measurement; `TCP_NODELAY` on; I/O timer armed once per stall window; store mutex kept (not a bottleneck);
  further optimisations are candidates awaiting the M2 measurements and approval.
- CI (Phase 9): Linux jobs on every push/PR, macOS only on main and on demand (private-repo cost: macOS minutes
  cost ~10× Linux); actions pinned to commit hashes; sanitizer jobs lower `vm.mmap_rnd_bits` to 28 (Ubuntu 24.04
  runners' 32 bits break GCC 13's sanitizer runtimes).
- GCC `-Wtsan` fence warning: accepted, not worked around (`BOOST_ASIO_DISABLE_FENCED_BLOCK` would change what is
  tested); no TSan reports observed; Clang TSan on macOS cross-checks.

## Build commands

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan && ctest --preset asan-ubsan
ctest --preset debug -L integration          # TCP tests only
cmake --preset ci && cmake --build --preset ci && ctest --preset ci   # what CI runs
scripts/coverage.sh                          # coverage (Linux, GCC)
cmake --preset bench && cmake --build --preset bench   # Release + micro-benchmarks + load generator
./build/bench/bench/cppkv_micro_bench && scripts/bench_load.sh
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
| 2026-10-30 | macOS | Phase 4: debug, asan-ubsan, tsan, ci | 134/134 each, no sanitizer reports |
| 2026-10-30 | Linux | Same 12 configurations | Phase 5: 146/146 in every configuration (incl. GCC TSan ×2); 0 warnings from CppKV code. Integration suite repeated 30× under TSan, 50× under ASan+UBSan, 200× Clang release: 0 failures, 0 sanitizer reports |
| 2026-10-30 | macOS | Phase 5: debug, asan-ubsan, tsan (Clang), ci | 146/146 each, no sanitizer reports |
| 2026-10-30 | Linux | Same 12 configurations | Phase 6: 180/180 in every configuration (incl. GCC TSan ×2, ASan+UBSan ×2); 0 warnings from CppKV code. Persistence + crash tests repeated 30× (GCC, Clang): 0 failures |
| 2026-10-31 | macOS | Phase 6: debug, asan-ubsan, tsan, ci | 180/180 each, no sanitizer reports |
| 2026-10-31 | Linux | Same 12 configurations | Phase 7: 199/199 in every configuration (the EMFILE test is skipped in the two ASan+UBSan builds, see Known issues); 0 warnings from CppKV code. Operations + process + persistence tests repeated 10× under TSan, 10× under ASan+UBSan, 20× Clang: 0 failures, 0 sanitizer reports |
| 2026-10-31 | macOS | Phase 7 (1st run): debug, asan-ubsan, tsan | 198/199 in debug and tsan: `AcceptBacksOff…` failed because macOS drops a connection whose `accept()` hit EMFILE (Linux keeps it queued). Test made platform-aware; Linux re-run 199/199, changed test 20/20 |
| 2026-10-31 | macOS | Phase 7 (after fix): debug, asan-ubsan, tsan, ci | 199/199 run, 0 failed, no sanitizer reports (wildcard-bind test skipped on macOS by design; EMFILE test also skipped in asan-ubsan) |
| 2026-10-31 | Linux | Same 12 configurations + `bench` (GCC/CMake 4.4/Boost 1.90, Clang/CMake 3.28/Boost 1.83) + ASan+UBSan and TSan with benchmarks — 16 total | Phase 8: 221/221 (222 with `bench.smoke` where benchmarks are built) in every configuration after one test fix (below); 0 warnings from CppKV code. Loadgen + operations tests repeated 10× under TSan and 10× under ASan+UBSan: 0 failures, 0 sanitizer reports. Two full 34-run load matrices: 0 errors |
| 2026-11-02 | macOS | Phase 8: debug, asan-ubsan, tsan, ci; then `bench` (micro + 34-run load matrix) | 221/221 (ci 222/222) run, 0 failed, no sanitizer reports (wildcard-bind skipped by design; EMFILE also skipped in asan-ubsan). Benchmarks: all 34 load runs 0 errors; results in docs/benchmarks.md §4M (one run, on battery, after the builds) |
| 2026-11-02 | Linux | The 5 CI job configurations (GCC/Clang `ci` and Clang `release` on CMake 3.28 + Boost 1.83 or 4.4 + 1.90; GCC ASan+UBSan+LSan and TSan on 3.28 + 1.83) | Phase 9: 234/234 (`ci`), 233/233 (release, sanitizers: no benchmark smoke test); 0 warnings from CppKV code; 0 sanitizer/leak reports. Workflow: actionlint clean. Not yet run on GitHub |

## Known issues

- GCC `tsan` builds print `-Wtsan` (`atomic_thread_fence` unsupported) from Asio's `std_fenced_block`; TSan does
  not model standalone fences. Investigate in Phase 5 before relying on GCC TSan.
- Clang sanitizer builds are untested on Linux (sandbox limitation); covered on macOS.
- Strong exception guarantee of `KvStore::set` is argued from the standard, not tested: testing it would need
  allocation-failure injection (a custom allocator), which is not worth the complexity yet.
- Unseeded string hash → hash-flooding risk if exposed to untrusted clients (documented in architecture §6).
- One store mutex serialises all commands, including the fsync under `--sync always` and the CRC of every logged
  record: measured in Phase 8 (benchmarks O5, O6); group commit / encoding outside the lock are candidates.
- Found by the Phase 8 matrix and fixed: `ProcessStats.PeakRssGrowsWhenMemoryIsTouched` failed in Clang release
  builds because the optimiser removed the never-read 64 MiB test allocation; it now writes through a volatile
  pointer.
- Benchmarks: the Linux numbers come from a 2-vCPU cloud VM where client and server share the CPUs; the M2 numbers
  are one run made on battery right after the test builds (to be repeated on mains power); open-loop p99 on the M2
  (up to 37 ms at 90 % load) is unexplained. `TCP_NODELAY` has no automated regression test (delayed-ACK timing is OS-specific); it is
  covered by the before/after measurement.
- Persistence limitations (ADR 0003): no compaction (log and restart time grow without bound); under
  `--sync always` every fsync blocks all commands (no group commit yet); power-loss durability is not testable
  here; `interval` is fixed at 1 s (not a CLI flag).
- `OperationsTest.AcceptBacksOffWhenOutOfFileDescriptorsAndRecovers` is skipped in UBSan builds: with no free file
  descriptor, UBSan's vptr check fails inside its own runtime ("invalid vptr … memory cannot be printed") — a
  sanitizer false positive. It runs in debug, release, ci and TSan builds.
- Found by the first Phase 7 matrix run and fixed: two tests read the server's log while I/O threads could still
  write to it (TSan: data race in `std::stringbuf`); they now stop the server first.
- Operations limits (ADR 0004): a client that ignores FIN keeps its socket for ≤ 2 s after the server is done;
  more than 64 KiB of input after the final reply still ends in RST; timeouts have 1 s granularity; no
  connection-rate limiting.

## Next phase

**Phase 10 — Portfolio release:** README rewritten for a reader who has never seen the project (what it is, design
in diagrams, measured results with their caveats, limitations), a scripted demo, a short "how to read this code"
guide, a release tag (with permission), and resume bullets limited to verified features. Before it: the first
GitHub Actions run must be green (owner pushes; failures are fixed here first).
