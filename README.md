# CppKV

An educational, Redis-inspired key-value database server written in modern C++20.

CppKV accepts TCP clients, speaks a small documented text protocol, and stores string keys and values in memory.
It is being built phase by phase toward concurrent clients, append-only-log recovery, sanitizer-clean tests,
CI on Linux and macOS, and honestly measured benchmarks.

CppKV is an independent learning project. It does not reuse Redis code and does not aim for Redis
protocol or feature compatibility.

## Status

**Phase 2 — in-memory key-value store.** The build system, a tested in-memory store (`KvStore`) and a minimal
`cppkv-server --version` executable exist; networking and the protocol do not yet.
See [`PROGRESS.md`](PROGRESS.md) for current status and [`docs/architecture.md`](docs/architecture.md) for the design.

## Planned feature set (v1.0)

- Commands: `PING`, `SET`, `GET`, `DEL`, `EXISTS`, `DBSIZE`, `QUIT`
- Line-based text protocol that correctly handles fragmented and pipelined TCP input
- Bounded resources: request size, connection count, per-client output buffering
- Concurrent clients with a documented, sanitizer-validated concurrency model
- Append-only log with CRC-checked records and a documented durability guarantee
- Graceful shutdown on SIGINT/SIGTERM
- Unit and integration tests (GoogleTest), micro- and end-to-end benchmarks (Google Benchmark + load generator)

Features appear in this list as *implemented* only once they are built and tested.

## Technology

C++20 · CMake · Boost.Asio · GoogleTest · Google Benchmark · ASan/UBSan/TSan · GitHub Actions

Targets: macOS on Apple Silicon (development) and Linux (validation).

## Documentation

- [`docs/architecture.md`](docs/architecture.md) — requirements, components, protocol draft, phase plan, risks
- `docs/protocol.md` — wire protocol specification (Phase 4)

## Build

### Requirements

| Tool | Minimum | Tested |
|------|---------|--------|
| C++20 compiler | GCC 13 / Clang 18 / Apple clang 21 | same |
| CMake | 3.25 | 3.28.3, 4.4.0 |
| Ninja | any recent | 1.11, 1.13 |
| Boost (headers only) | 1.83 | 1.83.0, 1.90.0 |

GoogleTest 1.17.0 is downloaded automatically during configuration (pinned by SHA-256), so the first
configure needs internet access.

```bash
# macOS (Homebrew)
brew install cmake ninja boost

# Ubuntu 24.04
sudo apt install build-essential cmake ninja-build libboost-dev
```

### Configure, build, test

```bash
cmake --preset debug          # configure into build/debug
cmake --build --preset debug  # compile
ctest --preset debug          # run all tests

./build/debug/src/cppkv-server --version
```

| Preset | Purpose |
|--------|---------|
| `debug` | Day-to-day development |
| `release` | Optimised build (benchmarks later) |
| `asan-ubsan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer (cannot be combined with ASan) |
| `ci` | Debug with warnings treated as errors |

**CLion:** open the folder; CLion picks up `CMakePresets.json` and lists the presets as CMake profiles
(enable the ones you want under *Settings → Build, Execution, Deployment → CMake*).

**Boost not found?** If CMake (or CLion) cannot find Homebrew's Boost, create a local, git-ignored
`CMakeUserPresets.json`:

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "debug-local", "inherits": "debug",
      "cacheVariables": { "CMAKE_PREFIX_PATH": "/opt/homebrew" } }
  ]
}
```
