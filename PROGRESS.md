# CppKV — Progress

## Current phase

**Phase 0 — Requirements and architecture: complete, awaiting approval.**

## Completed

### Phase 0 (2026-10-28)

- Inspected the development environments (below).
- Wrote `docs/architecture.md`: requirements, MVP and non-goals, component design, protocol v1 draft,
  store semantics, concurrency options, persistence draft, error-handling conventions, limits,
  repository layout, dependency strategy, macOS/Linux concerns, per-phase acceptance criteria, risks.
- Wrote `README.md` (status-only; no features claimed).

## Environment findings

### Linux validation workspace (cloud sandbox) — verified by running commands

| Item | Result |
|------|--------|
| OS / arch | Ubuntu 24.04.5 LTS, x86_64, 2 vCPUs, 7.8 GiB RAM |
| Compilers | GCC 13.3.0, Clang 18.1.3 |
| Build tools | CMake 3.28.3, Ninja 1.11.1, Git 2.43.0 |
| Sanitizers | `-fsanitize=address,undefined` and `-fsanitize=thread` compile, link and run (GCC) |
| Boost | Not installed; Ubuntu apt mirrors unreachable from the sandbox. Boost 1.86.0 release tarball downloads from GitHub (95 MB) — usable for sandbox validation. |
| GoogleTest | Not installed; GitHub reachable via git (`v1.17.0` tag visible) → `FetchContent` works. |

### macOS development machine (MacBook Air M2) — verified from owner's terminal output (2026-10-29)

| Item | Result |
|------|--------|
| OS / arch | macOS 27.0.1 (26A434), arm64 |
| Developer tools | Command Line Tools only (`/Library/Developer/CommandLineTools`); no full Xcode, so no Instruments |
| Compiler | Apple clang 21.0.0 (clang-2100.3.34.2) |
| Build tools | CMake 4.4.0, Ninja 1.13.2, Git 2.54.0, Homebrew 7.0.11 |
| Boost | 1.90.0_1 (Homebrew) |
| GoogleTest / Benchmark | Not installed via Homebrew → `FetchContent` (as planned) |
| `ulimit -n` | 2560 (Terminal) |
| `std::jthread` | Compiles and runs with `-std=c++20` (`JTHREAD_OK`) |

### Cross-platform checks run in the sandbox

- Boost 1.90.0 headers (Asio 1.38.0): `boost/asio/io_service.hpp` and `io_context::work` are gone, so only the modern Asio API can be used.
- GoogleTest `v1.17.0` declares `cmake_minimum_required(VERSION 3.16)`, so it works with CMake 4.
- Latest Google Benchmark tag upstream: `v1.10.0`.
- CMake 4 is not installable via pip in the sandbox; Phase 1 will try the official release binary to test the CMake 4 path here as well.

## Design decisions (Phase 0)

- Pure, socket-free framer / parser / dispatcher / encoder / store; only the session and server touch Asio.
- Line-based text protocol: LF terminator (optional CR stripped), single-space tokens, `SET` value is the rest of the line,
  `VALUE <len> <bytes>` responses, `ERR <CODE> <message>` errors.
- Boost headers only via CONFIG-mode `find_package` (supported range 1.83–1.90); GoogleTest and Google Benchmark via pinned `FetchContent`.
- `cmake_minimum_required(VERSION 3.25...4.4)` — covers Ubuntu 24.04 (3.28) and the Mac (4.4).
- Modern Asio API only (`io_context`, `make_work_guard`, free `asio::post`); no `io_service`/`io_context::work`.
- `std::jthread` allowed (verified on Apple clang 21 and available in GCC 13).
- Concurrency model deliberately deferred to Phase 5; persistence format drafted for Phase 6.

## Build commands

None yet (Phase 1).

## Test results

None yet (Phase 1).

## Known issues / open decisions

macOS toolchain: verified. Still open (`docs/architecture.md` §16): repository location on the Mac,
approval of dependency strategy / protocol draft / roadmap adjustments, licence.

## Next phase

**Phase 1 — Project skeleton and build system:** CMake + C++20, presets (debug/release/asan-ubsan/tsan),
warnings, Boost.Asio integration, GoogleTest + CTest, `.gitignore`, minimal executable, first passing test
on both Linux and macOS.
