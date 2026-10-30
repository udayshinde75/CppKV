# CppKV — Progress

## Current phase

**Phase 2 — In-memory key-value store: complete on Linux; awaiting macOS run and approval.**

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

## Build commands

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
cmake --preset asan-ubsan && cmake --build --preset asan-ubsan && ctest --preset asan-ubsan
```

## Test results

| Date | Platform | Configurations | Result |
|------|----------|----------------|--------|
| 2026-10-29 | Linux | GCC 13.3 / Clang 18.1 × CMake 3.28 / 4.4 × Boost 1.83 / 1.90; debug, release, ci, asan-ubsan (GCC), tsan (GCC) — 12 total | Phase 1: 10/10; Phase 2: 31/31 in every configuration; 0 warnings from CppKV code |
| 2026-10-29 | macOS | Apple clang 21, CMake 4.4, Boost 1.90; debug, asan-ubsan, tsan, ci | Phase 1: 10/10, no sanitizer reports, `ci` builds with `-Werror` |
| — | macOS | Phase 2 | **not yet run** |

## Known issues

- GCC `tsan` builds print `-Wtsan` (`atomic_thread_fence` unsupported) from Asio's `std_fenced_block`; TSan does
  not model standalone fences. Investigate in Phase 5 before relying on GCC TSan.
- Clang sanitizer builds are untested on Linux (sandbox limitation); covered on macOS.
- Strong exception guarantee of `KvStore::set` is argued from the standard, not tested: testing it would need
  allocation-failure injection (a custom allocator), which is not worth the complexity yet.
- Unseeded string hash → hash-flooding risk if exposed to untrusted clients (documented in architecture §6).

## Next phase

**Phase 3 — TCP server and client sessions:** synchronous Boost.Asio server, `PING`/`QUIT`, clean handling of
disconnects and network errors, `nc` examples, integration test on an ephemeral port.
