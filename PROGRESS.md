# CppKV — Progress

## Current phase

**Phase 1 — Project skeleton and build system: complete on Linux; awaiting macOS verification and approval.**

## Completed

### Phase 0 — Requirements and architecture (2026-10-28/29)

- Inspected Linux and macOS toolchains (below).
- `docs/architecture.md`: requirements, MVP/non-goals, components, protocol v1 draft, store semantics,
  concurrency options, persistence draft, error handling, limits, layout, dependencies, platform concerns,
  acceptance criteria, risks. Approved by the owner on 2026-10-29 (including sanitizer presets in Phase 1).

### Phase 1 — Project skeleton and build system (2026-10-29)

- CMake 3.25…4.4, C++20 without extensions, `CppKV::core` static library + `cppkv-server` executable.
- `CMakePresets.json`: `debug`, `release`, `asan-ubsan`, `tsan`, `ci` (warnings as errors).
- `cmake/CompilerWarnings.cmake` (per target), `cmake/Sanitizers.cmake` (directory-wide, validated combinations).
- Boost via CONFIG-mode `find_package(Boost 1.83)`, `Boost::headers` only, `BOOST_ASIO_NO_DEPRECATED`.
- GoogleTest 1.17.0 via `FetchContent` (release asset URL + SHA-256, `SYSTEM`), `gtest_discover_tests(PRE_TEST)`.
- Version generated from `project(VERSION)` into `cppkv/common/version.hpp`.
- `cppkv-server --version | --help`; unknown options exit with status 2.
- Tests: 5 unit tests (version decoding), 2 Asio smoke tests (post/run, loopback TCP on an ephemeral port),
  3 CLI tests (exact exit code checked by `tests/cmake/RunAndCheck.cmake`).
- `.gitignore`, README build instructions.

## Environment

### Linux validation workspace (cloud sandbox)

Ubuntu 24.04.5, x86_64, 2 vCPU. GCC 13.3.0, Clang 18.1.3, CMake 3.28.3 + 4.4.0 (official binary), Ninja 1.11.1.
Boost 1.83.0 and 1.90.0 installed from release tarballs (apt mirrors are unreachable).
Clang's sanitizer runtimes (`libclang-rt-18-dev`) are **not installed** and cannot be — so Clang sanitizer builds are
not tested here.

### macOS development machine (MacBook Air M2) — from owner's terminal output

macOS 27.0.1, arm64, Command Line Tools only (no Instruments), Apple clang 21.0.0, CMake 4.4.0, Ninja 1.13.2,
Boost 1.90.0 (Homebrew), `ulimit -n` 2560, `std::jthread` works with `-std=c++20`.

## Design decisions

- Pure, socket-free framer / parser / dispatcher / encoder / store; only session and server touch Asio.
- Protocol v1 draft: LF lines (optional CR stripped), single-space tokens, `SET` value = rest of line,
  `VALUE <len> <bytes>`, `ERR <CODE> <message>`.
- Supported Boost range 1.83–1.90; modern Asio API only, enforced by `BOOST_ASIO_NO_DEPRECATED`.
- Warnings applied per target; third-party headers are `SYSTEM`; `-Werror` only in `ci`.
- `-Wnull-dereference` Clang-only: GCC 13 + Boost 1.83 reported false positives inside Asio at `-O3`.
- Sanitizers apply to the whole build (including GoogleTest); UBSan is non-recoverable so tests fail on UB.
- One test executable per test type; tests discovered at CTest time, not build time.

## Build commands

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan && ctest --preset asan-ubsan
```

## Test results (Phase 1, Linux sandbox, 2026-10-29)

All 10 tests pass in each configuration; 0 warnings from CppKV code.

| Compiler | CMake | Boost | Presets |
|----------|-------|-------|---------|
| GCC 13.3 | 3.28.3 | 1.83.0 | debug, release, ci, asan-ubsan, tsan |
| GCC 13.3 | 4.4.0 | 1.90.0 | release, ci, asan-ubsan, tsan |
| Clang 18.1 | 3.28.3 | 1.83.0 | ci |
| Clang 18.1 | 4.4.0 | 1.90.0 | release, ci |

Sanitizer flags verified to detect a heap-buffer-overflow (ASan), a signed overflow (UBSan) and a data race (TSan) with GCC.

macOS (Apple clang 21, CMake 4.4, Boost 1.90): **not yet run** — owner to run the commands above.

## Known issues

- GCC `tsan` builds print `-Wtsan: 'atomic_thread_fence' is not supported with -fsanitize=thread` from Asio's
  `std_fenced_block`. TSan does not model standalone fences, so it could later report false races inside Asio.
  To be investigated in Phase 5 (compare with Clang TSan on macOS) before relying on GCC TSan results.
- Clang sanitizer builds untested on Linux in the sandbox (missing runtime); to be covered on macOS and in CI.

## Next phase

**Phase 2 — In-memory key-value store:** `KvStore` with set/get/del/exists/size, documented semantics,
exception-safety guarantees, comprehensive unit tests. No networking, no concurrency.
