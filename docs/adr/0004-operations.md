# ADR 0004 — Operational behaviour: shutdown, signals, timeouts, connection teardown

- **Status:** accepted (Phase 7)
- **Date:** 2026-10-31

## Context

Up to Phase 6, Ctrl-C killed the server outright. A client could hold a connection slot forever. The final reply
(`BYE`, `ERR TOO_LARGE`, `ERR BUSY`) could be destroyed by a TCP reset. Running out of file descriptors made the
accept loop spin, and a closed stderr pipe killed the process. Phase 7 makes each of these explicit and tested.

## Decisions

### 1. Signals → graceful shutdown

`TcpServer::handle_signals()` registers SIGINT and SIGTERM with `asio::signal_set`. The signal is delivered as an
ordinary completion handler on an I/O thread, so no server code runs inside a real signal handler, where
locking, allocating and logging are all unsafe.

| Event | Behaviour |
|-------|-----------|
| 1st SIGINT/SIGTERM | **Graceful:** stop accepting, then every connection answers the requests it has already received in full and closes. Partial requests are discarded (and logged). |
| still busy after `--shutdown-timeout` (default 10 s) | Remaining connections are closed forcibly. |
| 2nd SIGINT/SIGTERM | **Immediate:** close every connection now. |
| all connections gone | Timers and the signal wait are cancelled, `io_context::run()` returns on every thread, and `main` destroys the dispatcher, then the log (whose destructor syncs once more), then the store. Exit status 0. |

The shutdown state lives on the acceptor's strand (the "control strand"). Accept completions, timers, signal
handlers and shutdown steps are therefore serialised and need no locks. `SessionRegistry::close_and_collect()`
takes an `on_drained` callback that fires as soon as the last session releases its slot. Shutdown then ends at
once, instead of waiting for the full timeout.

**Draining never cancels a write.** A pending write carries replies to requests that have already been executed
(and, for `SET`, already logged). Cancelling it would mean a client never learns that its write succeeded. Only a
pending *read* is cancelled. `read_more()` then sees the drain flag and closes.

### 2. Lingering close

When the server ends a connection on purpose (`QUIT`, `TOO_LARGE`, `BUSY`, shutdown), it:

1. sends FIN after the last reply (`shutdown(send)`);
2. reads and discards whatever the client still sends, until EOF, 64 KiB, or 2 s;
3. closes.

Why: if a socket is closed while unread input is still queued, the kernel sends **RST** instead of FIN. On some
TCP stacks (e.g. Windows) an arriving RST discards data the client has received but not yet read, which can
include the final reply.

Measured on Linux (the client sends `QUIT` plus 30 KB more, 20 trials each):

| Server close | Client sees |
|--------------|-------------|
| bare `close()` | `BYE`, then **RST** (20/20) |
| `shutdown(both)` + `close()` (the immediate path) | `BYE`, then FIN — the RST, if any, comes after the FIN |
| lingering close | `BYE`, then FIN, no RST |

Linux delivered `BYE` in every case, so the extra protection that lingering adds is not observable on Linux.
The tests catch the bare-`close()` regression: three tests fail, because the client sees a reset instead of EOF.

Cost: a client that ignores the FIN keeps its socket open for up to 2 s after the server is done with it. The
connection slot is released before the FIN is sent, so lingering never blocks new clients.

### 3. I/O timeout (`--idle-timeout`, default 300 s, 0 = off)

If one read or write stays pending for the whole timeout without completing, the connection is closed. This
covers three cases with one mechanism:

- an idle client;
- a client that sends half a line and then nothing;
- a client that pipelines requests but never reads its replies.

The last two would otherwise hold a slot, and memory for one output batch, forever.

Two Asio details matter here:

- **Close, don't cancel.** `socket.cancel()` aborts only the operations queued in the reactor *at that moment*.
  An `async_write` is a chain of `write_some` steps, and between two steps nothing is queued, so a cancel is lost.
  The next step can then block forever with no timer left to stop it. This was found during Phase 7: a mutation
  that made the drain cancel writes went undetected, because the cancel kept missing the write. On timeout, the
  session therefore closes the socket, after which every pending or later operation fails.
- **Arm once, not per operation (changed in Phase 8).** Phase 7 re-armed a per-session `steady_timer` for every
  read and every write. Phase 8 measured that this cost ~30 % of throughput with one client on four I/O threads:
  two `timerfd_settime` calls per request, and re-arming from a thread other than the one blocked in
  `epoll_wait`/`kqueue` wakes that thread ([benchmarks](../benchmarks.md#o2)). Now starting an operation only
  records its start time. One wait stays pending; when it fires early (operations completed meanwhile) it re-arms
  itself for the *current* operation's deadline, `start + timeout`. The close happens at the same moment as
  before, and a stale completion needs no special check: the deadline is recomputed from the current operation.
  Three mutations (never close, no re-arm after progress, ignore progress) are each caught by the Phase 7 tests.

### 3a. `TCP_NODELAY` on every connection (added in Phase 8)

The server disables Nagle's algorithm on accepted sockets. With Nagle, a reply that is ready while an earlier reply
is still unacknowledged is held back until the ACK arrives; clients delay ACKs until they send again, so the
reply waits for the client's *next* request. Phase 8's open-loop test showed the signature: p95 latency equal to
the per-connection request interval (4 clients at 5000 req/s: 0.89 ms, at 2000 req/s: 2.1 ms). With `TCP_NODELAY`
the same runs gave 0.14 ms and 0.39 ms ([benchmarks](../benchmarks.md#o1)). Every reply is written in one
`async_write` per batch, so nothing is gained by letting the kernel coalesce. A failure to set the option is logged
at debug level and only costs latency. Not covered by an automated test: delayed-ACK timing differs between
operating systems, so a deterministic test is not practical; the measurement is the evidence.

### 4. Accept back-off on resource exhaustion

If `accept()` fails with `EMFILE`, `ENFILE`, `ENOBUFS` or `ENOMEM`, the next attempt waits 100 ms. Retrying at
once fails again at once, because the connection is still in the backlog, so the event loop spins at 100% CPU
and floods the log. Measured in the test: about 4 failures logged in 350 ms with the back-off, against an
unbounded number without it (the mutation trips the test's limit of 20). The server recovers by itself once
descriptors are available.

Platform difference (observed in the test): **Linux** keeps a connection queued while `accept()` fails, and serves
it after recovery. **macOS** drops it, so that client sees EOF or a reset, and no server can prevent that. On both,
new connections are served once descriptors are free, and the back-off bounds the retries.

### 5. SIGPIPE ignored process-wide

Asio suppresses SIGPIPE for its sockets, but not for stderr. Run as `cppkv-server 2>&1 | head -1`, the server
would die on the first log line after `head` exits. `main` sets `SIGPIPE` to `SIG_IGN`, so that write fails with
`EPIPE` and `std::cerr` absorbs it.

### 6. Logging and configuration

- `--log-level debug|info|warn|error`.
- The startup line lists every limit (threads, `max_connections`, `max_line_bytes`, timeouts).
- Every connection's close line carries `requests=` and `duration_ms=` plus the reason.
- Shutdown logs each step, and the final counters (`accepted=`, `rejected=`).
- `--max-line-bytes` (16 B – 8 MiB; the upper bound stays well inside the log's 16 MiB record limit),
  `--idle-timeout`, `--shutdown-timeout`.

## Exit status

| Status | Meaning |
|--------|---------|
| 0 | Clean shutdown (SIGINT/SIGTERM, graceful or forced) |
| 1 | Startup failure (port in use, unusable data directory, corrupted or locked log), or the log failed while running |
| 2 | Usage error |

## Consequences and limits

- A graceful shutdown takes at most `--shutdown-timeout`, plus up to 2 s of lingering for clients that ignore the FIN.
- A client that keeps sending more than 64 KiB after the server is done still receives an RST, by design: lingering
  is bounded.
- Timeouts have one-second granularity on the command line.
- Not covered: connection-rate limiting, per-client memory quotas beyond one output batch, TLS, authentication.

## Validation

- `OperationsTest` (in-process, real TCP):
  - the drain answers exactly the requests it received (the store size equals the number of replies) and ends with
    FIN;
  - idle clients do not delay shutdown;
  - the shutdown timeout forces stalled clients closed;
  - `BYE` and `ERR TOO_LARGE` arrive and end with FIN while the client is still sending;
  - the idle timeout closes a silent client and frees its slot;
  - a stalled reader is disconnected and its slot freed; an active client is not closed;
  - `EMFILE` via `RLIMIT_NOFILE`: back-off, and recovery.
- `ProcessTest` (the real binary):
  - SIGTERM → exit 0, idle client gets EOF, data kept;
  - SIGINT → exit 0;
  - a second signal stops immediately, despite a 60 s shutdown timeout;
  - the server survives a closed stderr pipe;
  - port in use → exit 1;
  - an unusable data directory → exit 1.
- Mutation checks, each caught:
  - bare close;
  - no accept back-off;
  - SIGPIPE not ignored;
  - the drain stopping without answering buffered requests;
  - no I/O timeout;
  - the timeout implemented with `cancel()`.
- Not caught on Linux, and not a test gap: lingering vs FIN-first (see §2).
