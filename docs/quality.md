# Quality

How CppKV is checked, what each check has been shown to catch, and what is known not to be covered. Every number
below comes from a command in this document, run on 2026-11-02 unless stated.

## 1. Continuous integration

[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) runs on clean GitHub-hosted runners:

| Job | Runner | Compiler | Preset | What it adds |
|-----|--------|----------|--------|--------------|
| Linux · GCC | `ubuntu-24.04` | GCC 13.3 | `ci` | `-Werror` with the full warning set; benchmarks compiled and smoke-run |
| Linux · Clang | `ubuntu-24.04` | Clang 18.1 | `ci` | The same with Clang's warnings (incl. `-Wnull-dereference`) |
| Linux · Clang Release | `ubuntu-24.04` | Clang 18.1 | `release` | `-O3`: optimiser-dependent behaviour (it found the test bug below) |
| Linux · ASan + UBSan + LSan | `ubuntu-24.04` | GCC 13.3 | `asan-ubsan` | Memory errors, undefined behaviour, **leaks** (LeakSanitizer exists only on Linux) |
| Linux · TSan | `ubuntu-24.04` | GCC 13.3 | `tsan` | Data races |
| macOS · AppleClang | `macos-26` (Apple Silicon) | Apple clang 21 | `ci` | The development platform: kqueue instead of epoll, `F_FULLFSYNC`, BSD socket behaviour |
| macOS · ASan+UBSan, TSan | `macos-26` | Apple clang 21 | `asan-ubsan`, `tsan` | On demand only (cost, see the workflow header) |

The Linux jobs install Boost from Ubuntu (1.83, the minimum CppKV supports); macOS gets the current Homebrew Boost,
so both ends of the supported range are built. Each job follows the README's instructions; nothing is configured
that a reader of the README would not do. Actions are pinned to commit hashes and the token is read-only. The
workflow passes [`actionlint`](https://github.com/rhysd/actionlint) 1.7.12.

**Not yet observed:** the workflow has not run on GitHub yet (it runs on the first push that contains it). Every
job's configuration was built and tested locally first (§2), but runner images differ from any local machine.

## 2. Local verification matrix

Before each phase is handed over, the suite runs in 12–16 configurations on Linux (GCC 13.3 / Clang 18.1 × CMake
3.28 / 4.4 × Boost 1.83 / 1.90 × debug, release, ci, ASan+UBSan, TSan, bench) and in debug, asan-ubsan, tsan and ci
on the MacBook Air M2. Results per phase: [`PROGRESS.md`](../PROGRESS.md#test-results).

## 3. Tests

234 CTest tests in the `ci` configuration: 158 unit (label `unit`), 69 integration (`integration`: real TCP to an
in-process server, real server processes killed with SIGKILL, signals, file-descriptor exhaustion), 6 command-line
tests of the two executables, 1 benchmark smoke test. Principles: tests synchronise on observable events, never on
sleeps; timeouts only detect hangs; every test gets fresh ports and directories, so they run in parallel.

### Sanitizers are known to work

A sanitizer job that never fires proves nothing, so each was shown to detect a real defect:

- **TSan:** removing the store lock produces 90 reports in `std::_Hashtable` and fails the 16-client test (Phase 5).
- **LeakSanitizer:** with the exact `ASAN_OPTIONS` of the `asan-ubsan` test preset, a deliberate 400-byte leak
  aborts the process with "LeakSanitizer: detected memory leaks" (exit 134); the full suite produces no such
  report. Each test runs in its own process (`gtest_discover_tests`), so a leak in any test fails that test.
- **UBSan** aborts instead of continuing (`-fno-sanitize-recover=undefined`); its one false positive is documented
  in PROGRESS.md (the EMFILE test is skipped under UBSan).

### Mutation checks

Coverage says what ran, not what was checked. So, phase by phase, the code was deliberately broken in ways a
reviewer might miss, and the suite had to fail. Every mutation below was caught:

| Phase | Mutations |
|-------|-----------|
| 2 | `insert_or_assign` → `try_emplace` (SET would not overwrite) |
| 4 | framer CR-at-limit rule; ignoring `closes_connection`; DEL via `contains`; trimming SET values; no output limit |
| 5 | store lock removed (TSan) |
| 6 | apply before log; DEL not logged; no partial-record rollback; strict recovery truncating; zero-tail rule removed; no fail-stop; no sync under `always` |
| 7 | bare `close()` (RST); no accept back-off; SIGPIPE not ignored; drain not answering buffered requests; no I/O timeout; timeout via `cancel()` |
| 8 | I/O timer: never closes; no re-arm after progress; ignores progress |
| 9 | load generator: two CSV columns swapped; a wrong GET reply not counted; an `ERR` reply to SET not counted |

Two mutations were *not* caught when first tried, and both led to fixes: Phase 7's "drain cancels writes" exposed
that `socket.cancel()` can miss a multi-step `async_write` (the timeout now closes instead), and the lingering
close is not observable on Linux at all (measured and documented in ADR 0004).

## 4. Coverage

`scripts/coverage.sh` (Linux, GCC `--coverage` + gcov) builds Debug, runs every test, and summarises the lines
and functions of `src/` and `tools/` that the tests executed — including the server processes the tests start.

| | Before Phase 9's tests | Now |
|-|-----------------------:|----:|
| **Total lines** (`src/` + `tools/`) | 90.8 % of 1 719 | **93.7 %** of 1 753 |
| **Total functions** | 96.6 % of 233 | **99.2 %** of 236 |
| `protocol/` (framer, parser, encoder) | 96.8–100 % | same |
| `store/`, `server/connection_handler`, `server/session_registry` | 100 % | same |
| `server/client_session`, `server/tcp_server` | 92.7 % | same |
| `persistence/append_log` / `posix_file` | 90.7 % / 77.2 % | same |
| `common/config` | 90.9 % | 99.0 % |
| `tools/loadgen/load_generator` | 79.9 % | 97.6 % |
| `tools/loadgen/main` (command line) | not run | 56.2 % (help and usage errors; a full run is covered through the library) |

What remains uncovered is almost entirely error handling that cannot be reached without fault injection:

| Not covered | Why | How it is checked instead |
|-------------|-----|---------------------------|
| `fsync` / `fdatasync` failure, `EINTR` retry loops, short reads (`posix_file.cpp`, `append_log.cpp`) | The OS does not fail these on demand; injecting would need a fake file layer | Write failure *is* injected (`RLIMIT_FSIZE`), which exercises the same fail-stop path; reviewed against ADR 0003 |
| Errors while opening/locking/initialising the log | Needs a file system that fails specific calls | Unusable data directory is tested end to end (exit status 1) |
| An exception inside request processing → `ERR INTERNAL` (`client_session.cpp`) | Would need a dispatcher that throws (e.g. `bad_alloc`) | Code review; the catch keeps the event loop alive |
| A failed `async_write`, a non-resource `accept` error, the closed-registry race (`tcp_server.cpp`) | Timing-dependent OS errors | Code review |
| Server exit after a log failure while running (`main.cpp`) | Needs a write failure inside a spawned process | The same failure is tested in-process (`AppendLogTest`) |
| Unreachable `default`/fallthrough branches | Defensive code | — |

## 5. Static analysis

`clang-tidy` 18 over every CppKV source in `src/` and `tools/` with `bugprone-*`, `clang-analyzer-*`,
`performance-*`, `concurrency-*`, `misc-*` and a few `cppcoreguidelines`/`modernize` checks. Findings in CppKV code
(third-party code excluded):

| Finding | Count | Action |
|---------|------:|--------|
| `bugprone-implicit-widening-of-multiplication-result` (`64 * 1024` computed in `int`) | 7 | **Fixed**: written as `std::size_t{64} * 1024`. Harmless at these values, but the pattern overflows silently with larger ones |
| `performance-move-const-arg` + `unnecessary-value-param` (`LogCorruptedError` moved a string into a `const&` constructor) | 2 | **Fixed** |
| `modernize-use-nodiscard` (`rest_is_zero`) | 1 | **Fixed** |
| `bugprone-unchecked-optional-access` (`signals_` in `wait_for_signal`) | 1 | Kept: only reachable after `handle_signals()` set it |
| `cppcoreguidelines-special-member-functions` (copy deleted, moves not declared) | 6 | Kept: deleting copy suppresses the implicit moves, so these classes are intentionally neither copyable nor movable |
| `misc-const-correctness` | 2 | Kept (cosmetic) |
| `performance-enum-size` | 16 | Kept: no enum is stored in bulk |

No `clang-analyzer` (path-sensitive) finding in CppKV code. Every finding in the categories `bugprone-use-after-move`,
`clang-analyzer-cplusplus.NewDeleteLeaks`, `concurrency-mt-unsafe` and `bugprone-narrowing-conversions` was inside
Google Benchmark or GoogleTest, not CppKV.

## 6. Phase 9 additions

The coverage audit found that the load generator's output was untested, although `scripts/bench_load.sh` reads its
CSV by column number: a reordered column would have silently put wrong numbers into `docs/benchmarks.md`. Added:
`LoadReportFormat.*` (column contract, values, text report), `LoadgenFakeServer.*` (wrong reply types and `ERR`
replies counted as errors; failed populate reported), the load generator's command-line exit status, and every
accepted value of the server's enumerated options.

## 7. Reproduce

```bash
cmake --preset ci && cmake --build --preset ci && ctest --preset ci        # what CI runs, plus -j for speed
scripts/coverage.sh                                                         # Linux + GCC
run-clang-tidy -p build/<a Clang build> -checks='-*,bugprone-*,clang-analyzer-*,performance-*' 'CppKV/(src|tools)/'
```
