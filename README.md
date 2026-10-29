# CppKV

An educational, Redis-inspired key-value database server written in modern C++20.

CppKV accepts TCP clients, speaks a small documented text protocol, and stores string keys and values in memory.
It is being built phase by phase toward concurrent clients, append-only-log recovery, sanitizer-clean tests,
CI on Linux and macOS, and honestly measured benchmarks.

CppKV is an independent learning project. It does not reuse Redis code and does not aim for Redis
protocol or feature compatibility.

## Status

**Phase 0 — requirements and architecture.** There is no code to build yet.
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

Build instructions will be added in Phase 1.
