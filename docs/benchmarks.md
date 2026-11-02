# Benchmarks

How CppKV's performance is measured, what was measured, and what the measurements led to.

**Rules this document follows.** Every number below was produced by the commands in §2 on the machine described
next to it. Numbers from different machines are never compared with each other, only within one machine. A
result is an observation about this code on that machine, not a general claim. No optimisation was made without
a measurement first; two were made after one (§5, O1 and O2), and the rest are listed as candidates (§6).

## 1. Tools

| Tool | What it measures | Source |
|------|------------------|--------|
| `cppkv_micro_bench` | Google Benchmark 1.10.0: store operations, framing/parsing/encoding, CRC-32 and log appends per sync policy, the whole per-request CPU path without sockets, the store mutex under 1–8 threads | `bench/` |
| `cppkv-loadgen` | A real TCP client: throughput and latency percentiles against a running server | `tools/loadgen/` |
| `scripts/bench_load.sh` | The load-test matrix (§4): a fresh server per run, one CSV row per run, plus the environment | `scripts/` |

### 1.1 Load generator

- **Closed loop** (default): each connection sends `--pipeline` requests (default 1), waits for all replies,
  repeats. Latency = reply time − send time. The offered load adapts to the server, so closed-loop latency at
  saturation mostly shows queueing (Little's law: latency ≈ clients × pipeline / throughput).
- **Open loop** (`--rate N`): each connection sends on a fixed schedule (N / clients per second, connections
  staggered) whatever the replies do; a second thread per connection reads replies. Latency is measured from the
  **scheduled** send time, so a client that falls behind its schedule (because the server is slow, or because
  the client itself woke up late) counts that delay as latency instead of hiding it ("coordinated omission"). The
  report shows the client's own **send lag** separately, so client-side delay can be told apart from server
  latency. If the achieved rate is below 95 % of the target, it prints a saturation warning.
- **Workload:** uniform random keys `key:0000000000…` over `--keys`, `--reads` % GET, the rest SET of
  `--value-bytes`. All keys are SET once before the run (`--populate`, on by default), so GETs hit. The request
  sequence comes from `mt19937_64` with plain modulo, so a seed produces the same requests on every platform.
- **Validation:** every reply is checked (OK for SET, VALUE/NIL for GET). Errors are counted and make the exit
  status 1; a lost connection or a refused one (`ERR BUSY`) fails the run instead of skewing it.
- **Latency histogram:** log-linear, in the style of HdrHistogram: exact up to 255 ns, then 128 buckets per power of
  two, so a reported percentile is never below the true value and at most 0.79 % above it; fixed 58 KiB; merged
  across connections. Unit-tested for gap-free bucket tiling over the whole `uint64_t` range, the error bound,
  and merge correctness (`LatencyHistogram.*`).
- **Measured window:** requests sent (closed loop) or scheduled (open loop) after `--warmup` and before the end of
  `--duration` are counted and timed; their replies may arrive just after the window.

The load generator itself is tested against an in-process server (`LoadgenTest.*`): its counts match the
server's state, the open loop sends exactly the scheduled number of requests, misses are counted, and a stopped
server or a connection limit is reported as a failure rather than as a result.

### 1.2 Server-side figures

On shutdown the server logs `peak_rss_kib`, `cpu_user_ms` and `cpu_system_ms` (from `getrusage`), and every
connection logs how many requests it served. `bench_load.sh` divides CPU by the exact number of requests served
(populate + warm-up + window) to get **CPU per request**, and the user/system split shows how much of the work is
CppKV's own code versus the kernel (TCP, epoll/kqueue, futexes, fsync).

## 2. How to reproduce

```bash
cmake --preset bench && cmake --build --preset bench      # Release -O3 + debug info

# Micro-benchmarks (3 repetitions; JSON for later comparison)
./build/bench/bench/cppkv_micro_bench --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
    --benchmark_out=micro.json --benchmark_out_format=json

# The load-test matrix: ~5 minutes; writes results.csv and environment.txt
scripts/bench_load.sh                       # into build/bench-results/<timestamp>/
DURATION=10 ONLY="e1 e2" scripts/bench_load.sh   # longer runs, selected experiments
```

The log benchmarks and `--sync` runs write to the temporary directory (`TMPDIR`) and `DATA_ROOT` respectively:
**that file system decides what fsync costs** (a tmpfs makes it free). `environment.txt` records which one it was.

## 3. Environments

| | Linux sandbox | MacBook Air M2 |
|-|---------------|----------------|
| CPU | Intel Xeon @ 2.10 GHz, **2 vCPUs** (cloud VM) | Apple M2, 4 performance + 4 efficiency cores, fanless |
| OS | Ubuntu 24.04, kernel 6.18 | macOS 27.0.1 |
| Compiler | GCC 13.3.0, `-O3 -DNDEBUG -g` | Apple clang 21.0.0, same flags |
| Boost | 1.90.0 (Asio 1.38.0) | 1.90.0 |
| Data file system | ext4 on a virtual disk (`/dev/vda`) | APFS on the internal SSD |
| Results | §4, measured 2026-10-31 | §4M, measured 2026-11-02 (one run; see the caveats there) |

Caveats for the Linux numbers: the load generator and the server share the same 2 vCPUs, so the client's CPU use
lowers the server's ceiling; loopback TCP is not a real network (no wire latency, no NIC); a cloud VM has noisy
neighbours and its fsync cost is the hypervisor's, not a physical disk's. Treat them as relative within §4.

## 4. Results: Linux sandbox

Each load run: 1 s warm-up, 5 s measured, 10 000 keys, 16-byte values unless stated, 4 I/O threads unless stated,
a fresh server per run. Latencies in µs. "sys" = share of server CPU spent in the kernel. Raw data (CSV, console
output, `strace` counts): [`bench-results/2026-10-31-linux-sandbox/`](bench-results/2026-10-31-linux-sandbox/).

### 4.1 Micro-benchmarks (medians of 3 repetitions)

| Operation | Time | Note |
|-----------|------|------|
| `KvStore::contains`, 1 000 keys | 29 ns | lookup only |
| `KvStore::contains`, 1 000 000 keys | 370 ns | table no longer fits in the caches |
| `KvStore::get` hit, 1 000 keys, 16 B / 1 KiB / 64 KiB value | 51 / 84 / 7 888 ns | lookup + copy of the value |
| `KvStore::get` hit, 1 000 000 keys, 16 B | 782 ns | |
| `KvStore::set` overwrite, 1 000 keys, 16 B / 1 KiB | 70 / 141 ns | |
| `parse_request` GET / SET 16 B / SET 16 KiB | 34 / 49 / 348 ns | |
| `LineFramer`, 1 000 pipelined GET lines | 9.2 µs (9 ns per line) | |
| `encode_response` OK / VALUE 16 B / VALUE 1 KiB | 5.6 / 23 / 38 ns | |
| CRC-32 (`boost::crc_32_type`), 64 B / 1 KiB / 64 KiB | 118 ns / 2.9 µs / 173 µs | **~350 MiB/s** |
| Encode one SET record, 16 B / 1 KiB value | 141 ns / 2.8 µs | dominated by the CRC |
| `AppendLog::append_set` 16 B, `--sync never` / `interval` | 0.58 / 0.57 µs | one `write(2)` |
| `AppendLog::append_set` 1 KiB, `--sync never` | 3.9 µs | 2.8 µs of it is the CRC |
| `AppendLog::append_set` 16 B / 1 KiB, `--sync always` | **161 / 235 µs** (CPU 41 / 54 µs) | one `fdatasync` each |
| Whole request path, no sockets (`ConnectionHandler`, 90 % GET), in memory / with log, `--sync never` | 138 / 192 ns per request | bounds what one thread can serve |
| `CommandDispatcher::execute` (90 % GET), 1 / 2 / 4 / 8 threads | 5.8 / 1.1 / 3.1 / 3.6 M ops/s in total | one mutex; 2 vCPUs |

### 4.2 Load tests (`bench_load.sh`, run 1)

**Repeatability.** The whole matrix was run twice in a row. Run 2's throughput differed from run 1's by −13 % to
+2 % per configuration (median −3.8 %), and every run of both had 0 errors. Tail latency varies more: at 75–90 %
open-loop load, p99 differed by up to 2.3×. So on this VM, **throughput differences below ~15 % between two
configurations are not meaningful**, and p99/p99.9 are indications, not precise values. Every conclusion in §5
rests on a larger effect or on repeated measurements. Tables show run 1.

**e1 — clients** (in memory, 90 % GET):

| Clients | Throughput (req/s) | p50 | p95 | p99 | p99.9 | CPU/request | sys |
|--------:|-------------------:|----:|----:|----:|------:|------------:|----:|
| 1 | 21 405 | 41 | 88 | 120 | 224 | 43 µs | 81 % |
| 2 | 32 655 | 53 | 127 | 187 | 537 | 34 µs | 82 % |
| 4 | 60 888 | 55 | 141 | 221 | 827 | 17 µs | 78 % |
| 8 | 85 559 | 70 | 207 | 500 | 2 212 | 11 µs | 77 % |
| 16 | 84 762 | 163 | 354 | 627 | 3 228 | 10 µs | 81 % |
| 32 | 79 564 | 365 | 729 | 1 327 | 4 031 | 11 µs | 75 % |
| 64 | 76 196 | 791 | 1 303 | 2 081 | 4 653 | 11 µs | 73 % |

Saturation at ~80–85 k req/s from 8 clients on; beyond that, more clients only add queueing (p50 at 64 clients ≈
64 / 76 196 s = 840 µs, as Little's law predicts). Peak RSS 5.1–5.6 MiB.

**e2 — durability** (16 clients):

| `--sync` | 90 % GET | 50 % GET | 0 % GET (SET only) |
|----------|---------:|---------:|-------------------:|
| `--in-memory` | 82 365 req/s, p99 627 | 84 204, p99 680 | 85 630, p99 594 |
| `never` | 85 702, p99 582 | 75 654, p99 709 | 76 836, p99 651 |
| `interval` | 77 005, p99 713 | 77 469, p99 696 | 73 340, p99 692 |
| `always` | **26 104**, p50 541, p99 2 015 | **8 424**, p50 1 770, p99 4 620 | **4 977**, p50 2 966, p99 7 111 |

**e3 — I/O threads** (in memory, 32 clients, 90 % GET): 1 / 2 / 4 / 8 threads → 82 462 / 77 287 / 84 461 / 87 961
req/s; CPU per request 8.6 / 11.2 / 10.1 / 10.7 µs. No gain from threads on 2 vCPUs.

**e4 — pipelining** (in memory, 4 clients, 90 % GET):

| Pipeline depth | Throughput | p50 | p99 | CPU/request | sys |
|---------------:|-----------:|----:|----:|------------:|----:|
| 1 | 58 703 | 58 | 222 | 18.5 µs | 75 % |
| 8 | 369 480 | 67 | 363 | 3.1 µs | 68 % |
| 32 | 915 315 | 119 | 412 | 1.5 µs | 61 % |

**e5 — open loop** (in memory, 16 clients, 90 % GET; closed-loop peak of this run 82 892 req/s):

| Target (= achieved) | % of peak | p50 | p95 | p99 | p99.9 |
|--------------------:|----------:|----:|----:|----:|------:|
| 20 723 | 25 % | 121 | 389 | 1 303 | 3 375 |
| 41 446 | 50 % | 297 | 1 004 | 2 081 | 8 585 |
| 62 169 | 75 % | 590 | 1 606 | 2 621 | 4 358 |
| 74 602 | 90 % | 811 | 2 114 | 3 293 | 5 800 |

**e6 — value size on the write path** (`--sync never`, 16 clients, SET only):

| Value | Throughput | p50 | p99 | CPU/request | sys | Peak RSS |
|------:|-----------:|----:|----:|------------:|----:|---------:|
| 16 B | 73 571 | 187 | 733 | 12.7 µs | 76 % | 5.5 MiB |
| 1 KiB | 46 010 | 287 | 1 581 | 21.4 µs | 67 % | 15 MiB |
| 16 KiB | 10 503 | 1 303 | 4 850 | 101 µs | 41 % | 176 MiB |

## 4M. Results: MacBook Air M2

Same commands and settings as §4. Raw data: [`bench-results/2026-11-02-macbook-air-m2/`](bench-results/2026-11-02-macbook-air-m2/).

**Caveats — read first.** This is **one** run, made straight after the four test builds: the micro-benchmarks
started at a load average of 63 and the load tests at 16, and the Mac was **on battery**. Load generator and server
share the 8 cores (4 performance, 4 efficiency). Differences below ~20 % between two configurations, single
outliers (e.g. e1 at 32 clients) and the open-loop tail latencies should not be read as findings until a second run
on mains power, after the machine has been idle, confirms them. Large effects (factors of 2 or more) are robust.

### 4M.1 Micro-benchmarks (medians of 3 repetitions)

| Operation | M2 | (Linux, §4.1) |
|-----------|---:|--------------:|
| `KvStore::contains`, 1 000 / 1 000 000 keys | 14 / 122 ns | 29 / 370 ns |
| `KvStore::get` hit, 1 000 keys, 16 B / 1 KiB / 64 KiB | 15 / 66 / 1 887 ns | 51 / 84 / 7 888 ns |
| `KvStore::set` overwrite, 1 000 keys, 16 B / 1 KiB | 17 / 63 ns | 70 / 141 ns |
| `parse_request` GET / SET 16 B / SET 16 KiB | 24 / 27 / 753 ns | 34 / 49 / 348 ns |
| CRC-32, 64 KiB | 391 MiB/s | ~360 MiB/s |
| `AppendLog::append_set` 16 B, `--sync never` | **2.47 µs** | 0.58 µs |
| `AppendLog::append_set` 16 B, `--sync always` (`F_FULLFSYNC`) | **2.69 ms** (CPU 104 µs) | 0.16 ms |
| Whole request path, no sockets, in memory / with log, `--sync never` | 84 / 277 ns per request | 138 / 192 ns |
| `CommandDispatcher::execute`, 1 / 2 / 4 / 8 threads | 15.3 / 8.9 / 6.8 / 4.7 M ops/s | 5.8 / 1.1 / 3.1 / 3.6 |

### 4M.2 Load tests (`bench_load.sh`)

**e1 — clients** (in memory, 90 % GET): 1 / 2 / 4 / 8 / 16 / 32 / 64 clients → 44 857 / 65 986 / 100 494 / 139 857 /
195 977 / 147 266 / 206 560 req/s; p50 22 / 25 / 36 / 52 / 78 / 166 / 297 µs; p99 52 / 81 / 85 / 140 / 175 / 754 /
696 µs. CPU per request 22 µs (1 client) → 10.5 µs (16+ clients), 81–86 % in the kernel. Peak RSS 3.2–3.7 MiB.

**e2 — durability** (16 clients):

| `--sync` | 90 % GET | 50 % GET | 0 % GET (SET only) |
|----------|---------:|---------:|-------------------:|
| `--in-memory` | 179 004 req/s, p99 215 | 187 241, p99 192 | 190 611, p99 180 |
| `never` | 173 690, p99 200 | **113 752**, p99 490 | **70 228**, p99 1 032 |
| `interval` | 123 725, p99 440 | 105 111, p99 541 | 89 342, p99 565 |
| `always` | **3 654**, p50 3 392, p99 15 598 | **894**, p50 17 039, p99 41 157 | **511**, p50 28 967, p99 79 692 |

**e3 — I/O threads** (in memory, 32 clients): 1 / 2 / 4 / 8 threads → **213 793 / 209 976 / 197 566 / 187 983** req/s,
CPU per request **4.4 / 6.6 / 11.1 / 14.3 µs**.

**e4 — pipelining** (in memory, 4 clients): depth 1 / 8 / 32 → 99 174 / 753 509 / **2 562 970** req/s; p99 102 / 106 /
108 µs; CPU per request 15.4 / 2.2 / 0.8 µs.

**e5 — open loop** (16 clients; closed-loop peak 170 114 req/s): at 25 / 50 / 75 / 90 % → p50 99 / 72 / 97 / 112 µs,
p99 909 µs / 5.0 ms / 29.6 ms / 37.2 ms, client send lag p50 72 / 41 / 31 / 28 µs. The median is flat while the
p99 grows by 40×; not explained yet (the 32 client threads and 4 server threads compete for 8 cores, half of them
efficiency cores) — to be re-measured.

**e6 — value size** (`--sync never`, SET only): 16 B / 1 KiB / 16 KiB → 72 537 / 69 119 / 11 104 req/s; CPU per
request 17.6 / 18.5 / 81.2 µs; kernel share 86 / 69 / 33 %; peak RSS 3.3 / 16 / 190 MiB.

## 5. Observations

<a id="o1"></a>
### O1. Nagle's algorithm held replies back — fixed

The first open-loop runs showed latency far above closed-loop latency at a *lower* load, with a telltale pattern:
p95 ≈ the interval between two requests on one connection. The server never set `TCP_NODELAY`. With Nagle, a
reply that is ready while an earlier reply is unacknowledged waits for the ACK, and the client delays its ACK until
it sends its next request. Fix: `TCP_NODELAY` on every accepted socket ([ADR 0004 §3a](adr/0004-operations.md)).

| Open loop, 4 clients | p50 before → after | p95 before → after | p99 before → after |
|----------------------|--------------------|--------------------|--------------------|
| 2 000 req/s (interval 2 ms per connection) | 205 → 196 | **2 146 → 385** | 2 245 → 725 |
| 5 000 req/s (interval 0.8 ms) | 131 → 88 | **893 → 141** | 934 → 238 |

Closed-loop results were unchanged (one request in flight never triggers Nagle), which is why the Phase 3–7 tests
could not see it. Any client that pipelines or sends in quick succession was affected.

<a id="o2"></a>
### O2. Re-arming the idle timer on every read and write — fixed

`strace -c -f` of the server with 1 client (closed loop) showed per request: 1 `recvfrom`, 1 `sendto`, 2 `epoll_wait`,
and **2 `timerfd_settime`** — the Phase 7 I/O timeout re-armed its timer for every operation. With 1 I/O thread
that cost nothing measurable; with 4 threads it did, because re-arming a timer from a thread other than the one in
`epoll_wait` must wake that thread:

| 1 client, 3 repetitions each | `--idle-timeout 300` (default) | `--idle-timeout 0` (no timer) |
|------------------------------|-------------------------------|-------------------------------|
| `--threads 1`, before | 22 530 – 24 712 req/s | 22 729 – 25 098 |
| `--threads 4`, before | **14 563 – 15 253** | 20 636 – 21 485 |
| `--threads 4`, after the fix | **21 017 – 21 417** | 20 642 – 21 403 |

Fix: an operation only records its start time; one timer wait stays pending and re-arms itself when it fires
early ([ADR 0004 §3](adr/0004-operations.md)). Same close time, and three mutations of the new logic are each
caught by the Phase 7 timeout tests. `timerfd_settime` no longer appears per request.

### O3. With one `io_context` on N threads, requests hop between threads

With one client (after O2, six 3-second repetitions each), `--threads 4` is no faster than `--threads 1`
(19.1–21.7 k vs 21.5–25.7 k req/s; the ranges overlap) but uses **~2.2× the CPU per request** (46–51 µs vs
21–24 µs, consistent in every repetition). `strace` shows why: **~14 `futex` calls per request** with 4 threads,
none with 1 — Asio hands each completion to whichever thread is free, and threads wake each other through
condition variables. With 32 clients on 2 vCPUs, 1–8 threads all give 77–88 k req/s (e3), so the extra threads
buy nothing there.

**On the 8-core M2 the answer is the same:** with 32 clients, 1 thread gives the most throughput (214 k req/s) at
the least CPU (4.4 µs per request); 8 threads give 188 k at **3.3× the CPU** (14.3 µs) (§4M.2 e3). Throughput
differences there are within the run's noise; the CPU cost is not. Candidate: one `io_context` per thread with
connections assigned round-robin, which removes the hand-offs and the strands
([ADR 0002](adr/0002-concurrency-model.md) option C×N). Until then, `--threads 1` is the efficient setting.

### O4. The work is in the kernel, not in CppKV's code

At pipeline depth 1, 73–87 % of the server's CPU is system time (e1, e2), and CPU per request is 10–40 µs while
the whole user-space request path costs ~0.14–0.19 µs (`ConnectionHandler` benchmarks) (§4.1). Pipelining 32 requests per round trip raises
throughput 15× (e4) because it divides the system calls by 32. Consequence: at depth 1, optimising the store, the
parser or the lock cannot raise throughput by more than the user-space share (~20 %).

### O5. `--sync always` is bounded by fsync, and reads wait behind it

One `fdatasync` on this virtual disk takes 160–235 µs (§4.1). Every SET pays one, inside the store lock (log order
= apply order, ADR 0003), so SETs are serialised at ~1 / fsync: **4 977 SET/s** (e2). Reads wait too: with 90 %
GET, throughput falls from 82 k to 26 k req/s, because a GET that arrives during a SET's fsync waits for the lock.
Candidates: **group commit** (one fsync for all SETs that arrived while the previous fsync ran), and not holding
the store lock across the fsync (apply under the lock, acknowledge after the shared fsync).

On the M2 the effect is ~10× larger: macOS's `fsync` does not flush the drive's cache, so CppKV uses
`F_FULLFSYNC` (ADR 0003), which took **2.7 ms** per append (§4M.1). SET-only throughput is **511 req/s**, and even a
90 % GET workload falls from 179 k to 3.7 k req/s with a p99 of 15.6 ms. On this machine `--sync always` is only
usable with group commit.

### O6. CRC-32 is two thirds of user-space CPU for large values — inside the lock

`boost::crc_32_type` runs at ~350 MiB/s here (byte-at-a-time table). A callgrind profile of the server under a
16 KiB SET-only load (`--sync never`, 8 clients) attributes **67.6 % of all user-space instructions to
`log_format::crc32`**, called from `AppendLog::append_set` inside `CommandDispatcher::execute`, i.e. under the store
mutex. e6 shows the effect: 16 KiB SETs reach 10.5 k req/s at 101 µs CPU each. Candidates, either keeping the log
format: compute the record (including its CRC) before taking the lock; and/or a faster CRC-32 of the same
polynomial (slicing-by-8, or the ARMv8 `crc32` instructions).

### O9. On macOS, the log's `write(2)` is expensive too — even without fsync

`AppendLog::append_set` with `--sync never` costs **2.47 µs** on the M2 (APFS) against 0.58 µs on Linux (ext4),
although the M2 is faster everywhere else (§4M.1). It runs inside the store lock. Under load the effect is large:
SET-only with `--sync never` reaches **70 k req/s against 191 k in memory** (−63 %; on Linux −10 %), and a 50 %
GET mix 114 k against 187 k (§4M.2 e2). Why a small `write(2)` costs this much on APFS was not profiled further
(Instruments needs full Xcode; `sample` is the next step). Group commit (O5) would also batch these writes:
one `write(2)` for many records.

### O7. GET's copy of the value is cheap; keep it

Architecture §6 asked whether `get()` returning a copy is a problem. Copy cost over a lookup: +21 ns (16 B), +53 ns
(1 KiB), +7.9 µs (64 KiB) (§4.1), against 10–40 µs of server CPU per request at depth 1 (O4). On the M2: +0.3 ns
(16 B fit libc++'s 22-byte short-string buffer, so no allocation), +52 ns, +1.9 µs. Not worth changing.

### O8. The single store mutex is not the bottleneck yet

In isolation the mutex contends badly (5.8 M ops/s on 1 thread → 1.1 M on 2, §4.1), but in the server it is held
for ~0.1–0.2 µs out of 10+ µs per request, and e3 shows no thread-count effect. It becomes the bottleneck only when
something slow runs inside it: the fsync (O5), the CRC (O6) and, on macOS, the `write(2)` (O9). On the M2 the
isolated mutex also loses throughput with every added thread (15.3 M → 4.7 M ops/s at 8 threads, §4M.1), still far
above the server's ~200 k req/s. ADR 0002's choice of one `std::mutex` stands.

## 6. Decisions

| Change | Evidence | Status |
|--------|----------|--------|
| `TCP_NODELAY` on accepted sockets | O1: p95 ÷ 6 at 5 000 req/s | **Done (Phase 8)** |
| Arm the I/O timer once, not per operation | O2: +42 % throughput, 1 client, 4 threads | **Done (Phase 8)** |
| Encode log records (CRC) outside the store lock | O6 | Candidate |
| Faster CRC-32, same polynomial | O6, §4.1 | Candidate |
| Group commit (one `write` + one fsync for many SETs) | O5 (×10 on the M2), O9 | Candidate — the largest measured effect |
| One `io_context` per thread | O3, confirmed on the 8-core M2 (3.3× CPU at 8 threads) | Candidate |
| Replace `std::mutex` with `shared_mutex` / shards | O8: no evidence it limits anything | **Rejected for now** |
| Return views instead of copies from `get()` | O7 | **Rejected** |

## 7. Not yet measured

- **A second M2 run** on mains power after the machine has been idle, to confirm §4M and the open-loop tail (e5).
- A real network between client and server; skewed (Zipf) key popularity; runs long enough to see thermal
  throttling on the fanless MacBook Air; memory per key at millions of keys.
