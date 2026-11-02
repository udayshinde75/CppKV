#!/usr/bin/env bash
# Runs the Phase 8 load-test matrix against freshly started servers and writes
# one CSV row per run, plus a record of the machine it ran on.
#
#   scripts/bench_load.sh [output-dir]          (default: build/bench-results/<timestamp>)
#
# Environment overrides:
#   BUILD=build/bench   where cppkv-server and cppkv-loadgen were built (preset "bench")
#   DURATION=5          measured seconds per run      WARMUP=1   warm-up seconds per run
#   KEYS=10000          key space (populated before every run)
#   DATA_ROOT=<out>/data  where the server's log goes: this decides what fsync costs
#   ONLY="e1 e2"        run only these experiments (e1..e6)
#
# Every run starts a new server with an empty data directory, so runs do not
# influence each other through log size or memory. Portable to macOS bash 3.2.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build/bench}
DURATION=${DURATION:-5}
WARMUP=${WARMUP:-1}
KEYS=${KEYS:-10000}
OUT=${1:-$ROOT/build/bench-results/$(date +%Y%m%d-%H%M%S)}
DATA_ROOT=${DATA_ROOT:-$OUT/data}
ONLY=${ONLY:-"e1 e2 e3 e4 e5 e6"}

SERVER=$BUILD/src/cppkv-server
LOADGEN=$BUILD/tools/loadgen/cppkv-loadgen
for binary in "$SERVER" "$LOADGEN"; do
    if [ ! -x "$binary" ]; then
        echo "error: $binary not found; build first: cmake --preset bench && cmake --build --preset bench" >&2
        exit 2
    fi
done
mkdir -p "$OUT/logs" "$DATA_ROOT"
CSV=$OUT/results.csv

# --- Environment -------------------------------------------------------------
{
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "git: $(git -C "$ROOT" describe --always --dirty 2>/dev/null || echo unknown)"
    echo "uname: $(uname -a)"
    if [ "$(uname)" = Darwin ]; then
        echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
        echo "logical_cpus: $(sysctl -n hw.ncpu)"
        echo "performance_cores: $(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo ?)"
        echo "efficiency_cores: $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo ?)"
        echo "memory_bytes: $(sysctl -n hw.memsize)"
        echo "power: $(pmset -g batt 2>/dev/null | head -1)"
        echo "data_fs: $(df -h "$DATA_ROOT" | tail -1)"
        echo "data_mount: $(mount | grep " on $(df "$DATA_ROOT" | tail -1 | awk '{print $NF}') " || true)"
    else
        echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
        echo "logical_cpus: $(nproc)"
        echo "memory: $(grep MemTotal /proc/meminfo)"
        echo "data_fs: $(df -hT "$DATA_ROOT" | tail -1)"
    fi
    echo "open_file_limit: $(ulimit -n)"
    echo "load_average_before: $(uptime | sed 's/.*load average[s]*: //')"
    echo "server: $("$SERVER" --version)"
    grep -E '^CMAKE_(CXX_COMPILER|BUILD_TYPE|CXX_FLAGS_RELEASE):' "$BUILD/CMakeCache.txt" 2>/dev/null || true
    echo "duration_s: $DURATION  warmup_s: $WARMUP  keys: $KEYS"
} > "$OUT/environment.txt"
cat "$OUT/environment.txt"

# --- One run -----------------------------------------------------------------
RUN=0
HEADER_WRITTEN=0
LAST_THROUGHPUT=0

# run <experiment> <server threads> <sync: memory|never|interval|always> <loadgen args...>
run() {
    local experiment=$1 threads=$2 sync=$3
    shift 3
    RUN=$((RUN + 1))
    local log="$OUT/logs/run-$RUN.log" data="$DATA_ROOT/run-$RUN"
    local server_args=(--port 0 --threads "$threads" --max-connections 1000)
    if [ "$sync" = memory ]; then
        server_args+=(--in-memory)
    else
        server_args+=(--data-dir "$data" --sync "$sync")
    fi

    "$SERVER" "${server_args[@]}" 2> "$log" &
    local pid=$!
    local port="" tries=0
    while [ -z "$port" ] && [ $tries -lt 100 ]; do
        sleep 0.05
        port=$(sed -n 's/.*listening on [^ ]*:\([0-9][0-9]*\) .*/\1/p' "$log")
        tries=$((tries + 1))
    done
    if [ -z "$port" ]; then
        echo "error: server did not start; see $log" >&2
        kill "$pid" 2>/dev/null || true
        exit 1
    fi

    local label="$experiment/run$RUN"
    local output status=0
    output=$("$LOADGEN" --port "$port" --duration "$DURATION" --warmup "$WARMUP" --keys "$KEYS" \
        --format csv --label "$label" "$@") || status=$?
    kill -TERM "$pid"
    wait "$pid" || true

    # Server-side figures from its log: memory, CPU, and the exact number of
    # requests it served (populate + warm-up + window), summed per connection.
    local rss user system served
    rss=$(sed -n 's/.*peak_rss_kib=\([0-9]*\).*/\1/p' "$log")
    user=$(sed -n 's/.*cpu_user_ms=\([0-9]*\).*/\1/p' "$log")
    system=$(sed -n 's/.*cpu_system_ms=\([0-9]*\).*/\1/p' "$log")
    served=$(awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^requests=/) { sub("requests=", "", $i); s += $i } } END { print s + 0 }' "$log")
    local cpu_us_per_request
    cpu_us_per_request=$(awk -v u="$user" -v s="$system" -v n="$served" 'BEGIN { if (n > 0) printf "%.2f", (u + s) * 1000 / n; else print "" }')

    if [ $HEADER_WRITTEN = 0 ]; then
        echo "experiment,server_threads,sync,$(echo "$output" | head -n 1),server_peak_rss_kib,server_cpu_user_ms,server_cpu_system_ms,server_requests,server_cpu_us_per_request" > "$CSV"
        HEADER_WRITTEN=1
    fi
    local row
    row=$(echo "$output" | tail -n 1)
    echo "$experiment,$threads,$sync,$row,$rss,$user,$system,$served,$cpu_us_per_request" >> "$CSV"
    # throughput_rps is column 11 of the loadgen row
    LAST_THROUGHPUT=$(echo "$row" | cut -d, -f11)
    printf '%-4s run %-3s threads=%-2s sync=%-8s %-48s -> %8s req/s  p50 %7s us  p99 %8s us  rss %6s KiB  cpu/req %6s us\n' \
        "$experiment" "$RUN" "$threads" "$sync" "$*" "$LAST_THROUGHPUT" \
        "$(echo "$row" | cut -d, -f12)" "$(echo "$row" | cut -d, -f14)" "$rss" "$cpu_us_per_request"
    if [ $status -ne 0 ]; then
        echo "warning: load generator exited with status $status (see the 'failed' and 'errors' columns)" >&2
    fi
    rm -rf "$data"
}

wanted() { case " $ONLY " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

# --- Experiments ---------------------------------------------------------------
# e1: how throughput and latency change with the number of clients.
if wanted e1; then
    for clients in 1 2 4 8 16 32 64; do run e1 4 memory --clients "$clients" --reads 90; done
fi
# e2: the cost of durability, for read-heavy, mixed and write-only workloads.
if wanted e2; then
    for reads in 90 50 0; do
        for sync in memory never interval always; do run e2 4 "$sync" --clients 16 --reads "$reads"; done
    done
fi
# e3: does adding I/O threads help? (one store mutex, ADR 0002)
if wanted e3; then
    for threads in 1 2 4 8; do run e3 "$threads" memory --clients 32 --reads 90; done
fi
# e4: pipelining amortises per-request system calls.
if wanted e4; then
    for depth in 1 8 32; do run e4 4 memory --clients 4 --pipeline "$depth" --reads 90; done
fi
# e5: latency at a fixed offered load (open loop), relative to the closed-loop peak.
if wanted e5; then
    run e5 4 memory --clients 16 --reads 90
    peak=$LAST_THROUGHPUT
    for percent in 25 50 75 90; do
        rate=$(awk -v p="$peak" -v f="$percent" 'BEGIN { printf "%d", p * f / 100 }')
        run e5 4 memory --clients 16 --reads 90 --rate "$rate"
    done
fi
# e6: value size on the write path (log encoding + CRC + write), no fsync.
if wanted e6; then
    for bytes in 16 1024 16384; do run e6 4 never --clients 16 --reads 0 --value-bytes "$bytes"; done
fi

rmdir "$DATA_ROOT" 2>/dev/null || true
echo
echo "results:     $CSV"
echo "environment: $OUT/environment.txt"
echo "server logs: $OUT/logs/"
