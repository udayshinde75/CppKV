# CppKV

An educational, Redis-inspired key-value database server written in modern C++20.

CppKV accepts TCP clients, speaks a small documented text protocol, and stores string keys and values in memory.
It is being built phase by phase toward concurrent clients, append-only-log recovery, sanitizer-clean tests,
CI on Linux and macOS, and honestly measured benchmarks.

CppKV is an independent learning project. It does not reuse Redis code and does not aim for Redis
protocol or feature compatibility.

## Status

**Phase 6 — persistence.** `cppkv-server` serves many TCP clients concurrently on a fixed pool of I/O threads and
supports `PING`, `SET`, `GET`, `DEL`, `EXISTS`, `DBSIZE` and `QUIT` over a documented text protocol that handles
fragmented and pipelined input. Every change is written to a checksummed append-only log before it is acknowledged,
so data survives restarts and crashes, with the guarantee per sync policy documented in
[ADR 0003](docs/adr/0003-persistence.md).
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
- [`docs/adr/`](docs/adr/) — architecture decision records: server model, concurrency, persistence and durability
- [`docs/protocol.md`](docs/protocol.md) — wire protocol specification: requests, responses, errors, limits

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
ctest --preset debug -L integration   # only the TCP tests (labels: unit, integration)

./build/debug/src/cppkv-server --version
```

| Preset | Purpose |
|--------|---------|
| `debug` | Day-to-day development |
| `release` | Optimised build (benchmarks later) |
| `asan-ubsan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer (cannot be combined with ASan) |
| `ci` | Debug with warnings treated as errors |

## Run

```bash
./build/debug/src/cppkv-server                  # listens on 127.0.0.1:7379
./build/debug/src/cppkv-server --port 0         # any free port; the log line shows which
./build/debug/src/cppkv-server --bind ::1 --port 7380
./build/debug/src/cppkv-server --threads 8 --max-connections 500
./build/debug/src/cppkv-server --data-dir /tmp/kv --sync interval
./build/debug/src/cppkv-server --in-memory      # no persistence
```

By default the log is `./data/cppkv.log`, relative to the directory you start the server in, and every write is
`fsync`ed before `OK` (`--sync always`). Try it: `SET` something, stop the server (even with `kill -9`), start it
again and `GET` it back. If the log is damaged anywhere except its last record, the server refuses to start and
says where; `--recovery truncate` keeps the valid part instead.

Stop it with Ctrl-C. In a second terminal, talk to it with `nc` (installed on macOS and most Linux systems):

```console
$ nc 127.0.0.1 7379
PING
PONG
SET city Frankfurt am Main
OK
GET city
VALUE 17 Frankfurt am Main
GET nope
NIL
EXISTS city
INT 1
DBSIZE
INT 1
DEL city
INT 1
FLY away
ERR UNKNOWN_COMMAND unknown command 'FLY'
QUIT
BYE
```

`VALUE <len> <bytes>` carries the exact byte length. Full rules: [`docs/protocol.md`](docs/protocol.md).
Two scripted demos:

```bash
# Several requests in one write: one reply each
printf 'SET a 1\r\nSET b 2\r\nDBSIZE\r\nQUIT\r\n' | nc 127.0.0.1 7379

# One request split across writes: the server reassembles it
(printf 'SET na'; sleep 1; printf 'me hello\nGET name\n'; sleep 1; printf 'QUIT\n') | nc 127.0.0.1 7379
```

Open several `nc` sessions at once: they are served concurrently and see each other's writes. Once
`--max-connections` clients are connected, the next one gets `ERR BUSY too many connections`.
The design is explained in [ADR 0002](docs/adr/0002-concurrency-model.md).

## IDE notes

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
