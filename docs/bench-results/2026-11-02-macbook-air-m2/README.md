# Raw results: MacBook Air M2, 2026-11-02

The data behind §4M of [`../../benchmarks.md`](../../benchmarks.md). Apple M2 (4 performance + 4 efficiency
cores), 8 GiB, macOS 27, Apple clang 21, Boost 1.90, `bench` preset (`-O3 -DNDEBUG -g`), data on APFS on the
internal SSD (`--sync always` uses `F_FULLFSYNC`). Client and server ran on the same machine.

| File | Content |
|------|---------|
| `environment.txt` | What `scripts/bench_load.sh` recorded before the load tests |
| `load-run1.csv` | The 34-run load matrix |
| `micro.json` | `cppkv_micro_bench --benchmark_repetitions=3`, Google Benchmark JSON (includes its own context) |

Conditions: one run, started right after the test builds (load average 63 when the micro-benchmarks started, 16
when the load tests started), on battery power. See the caveats in §4M before comparing numbers.
