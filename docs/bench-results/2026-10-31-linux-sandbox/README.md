# Raw results: Linux sandbox, 2026-10-31

The data behind the Linux tables and observations in [`../../benchmarks.md`](../../benchmarks.md).
2 vCPU cloud VM (Intel Xeon @ 2.10 GHz), Ubuntu 24.04, kernel 6.18, GCC 13.3, Boost 1.90, `bench` preset
(`-O3 -DNDEBUG -g`), data on ext4 on a virtual disk. Client and server ran on the same VM.

| File | Content |
|------|---------|
| `environment.txt` | What `scripts/bench_load.sh` recorded before run 1 |
| `load-run1.csv`, `load-run2.csv` | The 34-run matrix, twice in a row (§4.2 tables use run 1) |
| `micro.txt` | `cppkv_micro_bench --benchmark_repetitions=3` (§4.1) |
| `o1-nagle-before.csv`, `o1-nagle-after.csv` | O1: open loop before/after `TCP_NODELAY` |
| `o2-timer.txt` | O2 and O3: timer before/after, `strace -c` counts, CPU per request by thread count |

Order of events: the micro-benchmarks and O1 were measured first; then the O2 fix; then both load runs. Neither
fix touches code that the micro-benchmarks exercise (they use no sockets or timers).
