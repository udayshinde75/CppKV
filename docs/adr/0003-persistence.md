# ADR 0003 — Persistence: append-only log with checksummed records

- **Status:** accepted (Phase 6)
- **Date:** 2026-10-30

## Context

The store is in memory. Phase 6 must make data survive restarts and crashes, with a durability guarantee that
is stated precisely and tested — and without claiming more than is actually guaranteed.

## Decision

An append-only log `<data-dir>/cppkv.log` (default `./data/cppkv.log`), written by `AppendLog`.

**Write path** (`CommandDispatcher::execute`, under its mutex):

1. Encode the mutation (`SET` or a `DEL` of an existing key) as one record.
2. `write()` it to the end of the log (`O_APPEND`).
3. Sync according to `--sync`.
4. Only if 2 and 3 succeeded, apply the change to memory.
5. The session sends `OK` / `INT 1`.

Because the log append and the memory update share one lock, the order of records in the log is exactly the
order in which clients could observe the changes. A `DEL` of a missing key changes nothing and is not logged.

**Recovery** (`AppendLog::open`, before the server accepts connections): replay every valid record in order
into the empty store, repair a damaged tail, refuse to start on damage elsewhere.

## File format

```
file    := header record*
header  := "CPPKVLOG" u16:version(=1)                        10 bytes
record  := u32:payload_len u32:crc32(payload) payload
payload := u8:op(1=SET, 2=DEL) u32:key_len key [u32:value_len value]
```

All integers are little-endian and written byte by byte, so a log is portable between arm64 macOS and x86-64
Linux. The CRC is standard CRC-32 (Boost.CRC); golden-byte tests pin the format against values computed
independently with Python's `zlib`. A payload is at most 16 MiB, so a corrupted length field cannot trigger a huge
allocation during recovery.

## What `OK` guarantees

| `--sync` | When `OK` is sent, the record is… | Survives process crash (`kill -9`, segfault) | Survives OS crash / power loss |
|----------|-----------------------------------|:---:|:---:|
| `always` (default) | written **and** flushed to the storage device (`fdatasync` on Linux; `fcntl(F_FULLFSYNC)` on macOS) | yes | yes¹ |
| `interval` | written to the OS; flushed by a background thread about once per second | yes | all but the last ~1 s |
| `never` | written to the OS; flushed whenever the OS decides | yes | no guarantee |

¹ Assuming the device honours flush requests. Some consumer drives acknowledge a flush before the data is on
stable media; no software can detect that.

**Acknowledgement and durability are different things.** "The server sent `OK`" means the server finished
its write path. Whether the data then survives a power loss depends on the policy above. Under `never`, a
client can receive `OK` for a write that is lost when the machine loses power a moment later.

The implication goes one way only: **OK ⇒ durable (per policy)**. "No OK" does **not** imply "not applied". If the
server crashes after step 2 or 3 but before the client reads `OK`, the record is replayed on restart. A client
that retries a `SET` after a lost connection is safe, because `SET` is idempotent.

Why macOS needs `F_FULLFSYNC`: on macOS, `fsync()` hands data to the drive but does not ask the drive to empty
its volatile write cache. `fcntl(F_FULLFSYNC)` does. CppKV falls back to `fsync()` only where the file system
reports that `F_FULLFSYNC` is unsupported (`ENOTSUP`, `ENOTTY` or `EINVAL`), never on a real I/O error.

## What happens if the process dies half-way through a record

`write()` may have put any prefix of the record into the file. On restart, the replay finds the record cut
short and classifies the damage:

| Found at offset X | Classified as | Startup |
|-------------------|---------------|---------|
| fewer than 8 bytes left (incomplete record header) | torn tail | truncate at X, continue |
| record extends past end of file | torn tail | truncate at X, continue |
| complete record with bad CRC or bad structure, **and it is the last record** | torn tail | truncate at X, continue |
| only zero bytes from X to end of file² | torn tail | truncate at X, continue |
| length field larger than 16 MiB (and not zeros) | **corruption** | refuse (strict) |
| bad CRC or structure **followed by more non-zero data** | **corruption** | refuse (strict) |

² Some file systems update the file size before the data blocks are written. After a power loss, that region
reads back as zeros.

The reasoning: a crash can only damage the **end** of an append-only file. Damage *followed by valid-looking
data* has another cause (a bug, bit rot, a manual edit). "Repairing" it by truncating would silently discard
writes that were acknowledged after the damaged record. So the default `--recovery strict` refuses to start,
names the offset and leaves the file untouched for inspection. `--recovery truncate` is an explicit operator
decision to keep the valid prefix.

A file that does not start with `CPPKVLOG` (or a prefix of it) is never modified, in either mode.

## Failure while running: fail-stop

If a `write()` fails part-way through a record (disk full, file size limit, I/O error), the partial record is
removed with `ftruncate()`. If `fdatasync` fails, the log is marked failed. In both cases, every later mutation
is refused with `ERR INTERNAL write not persisted (...)`; reads keep working. Recovery requires a restart.

A failed sync is not retried. After a failed `fsync`, Linux may already have dropped the dirty pages, so a retry
can report success for data that never reached the disk (PostgreSQL's 2018 "fsyncgate"). Stopping writes is the
only safe reaction.

## Exclusive access

The log is locked with `flock(LOCK_EX | LOCK_NB)`. A second server on the same data directory fails at startup
("in use by another process") instead of interleaving records. The lock is released by the kernel when the
process exits, including on `kill -9`.

## Alternatives considered

- **Periodic snapshots only:** simpler, but every write since the last snapshot is lost on a crash, and writing
  a snapshot of a large store blocks or needs copy-on-write.
- **Write-ahead log + snapshots + compaction:** the complete solution. Compaction (rewriting the log as one
  record per live key) is out of scope for v1, so **the log grows without bound** and restart time grows with it.
- **`std::fstream`:** has no fsync, no file locking and no truncate, so it cannot provide the guarantees above.
- **`mmap`:** adds complexity around `msync` and `SIGBUS` on I/O errors, with no benefit at this scale.
- **Group commit** (one fsync for several clients' records): would raise throughput under `--sync always`, where
  today every fsync blocks all commands behind the dispatcher lock. Phase 8 measures before deciding.

## Validation

- `log_format_test`: golden bytes, CRC check value, round trips (including NUL and CR/LF bytes), malformed payloads.
- `append_log_test`:
  - replays SET, DEL and overwrites, and appending continues after recovery;
  - **the file cut at every byte offset** recovers exactly the complete records, and stays appendable;
  - zero-filled tail; a damaged last record;
  - mid-file corruption: strict refuses and leaves the file unchanged, truncate keeps the prefix;
  - absurd length field; foreign files; unsupported version; partial header;
  - the `flock` lock;
  - sync counts per policy;
  - **real write-failure injection** via `RLIMIT_FSIZE`: the partial record is rolled back, writes stop and
    reads continue.
- `persistence_test` (integration):
  - data survives a server restart; 800 concurrent writes are all recovered;
  - **the real binary killed with SIGKILL** recovers every acknowledged write (`--sync always`, and `never`);
  - a torn tail is repaired on startup;
  - a corrupted log makes the binary exit with status 1, and `--recovery truncate` then starts it;
  - a second server on the same data directory is refused.
- `strace` on Linux: under `--sync always`, 20 SETs cause exactly 20 `fdatasync` calls (plus 2 at log creation);
  under `interval` and `never`, none during the burst.
- **Not testable here:** power loss. The guarantee in the last table column rests on the OS and the device
  honouring `fdatasync` / `F_FULLFSYNC`, which needs a power-cut rig to verify.
