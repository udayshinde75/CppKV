#!/usr/bin/env bash
# A two-minute tour of CppKV: commands, pipelining, a request split across
# writes, crash recovery after kill -9, graceful shutdown, and a short load test.
#
#   scripts/demo.sh                 # uses build/debug (build it first: see README)
#   BUILD=build/bench scripts/demo.sh
#
# Talks to the server with nothing but bash (/dev/tcp), so it needs no client
# tools. Everything happens in a temporary directory, removed at the end.
# Portable to macOS bash 3.2.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$ROOT/build/debug}
SERVER=$BUILD/src/cppkv-server
LOADGEN=$BUILD/tools/loadgen/cppkv-loadgen
for binary in "$SERVER" "$LOADGEN"; do
    if [ ! -x "$binary" ]; then
        echo "error: $binary not found; build first: cmake --preset debug && cmake --build --preset debug" >&2
        exit 2
    fi
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/cppkv-demo.XXXXXX")
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -KILL "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

if [ -t 1 ]; then BOLD=$'\033[1m'; DIM=$'\033[2m'; RESET=$'\033[0m'; else BOLD=""; DIM=""; RESET=""; fi
step() { printf '\n%s== %s ==%s\n' "$BOLD" "$1" "$RESET"; }
note() { printf '%s%s%s\n' "$DIM" "$1" "$RESET"; }

PORT=""
# start_server <log file> <server arguments...>: starts it, waits for "listening on", sets PORT.
start_server() {
    local log=$1
    shift
    "$SERVER" --port 0 "$@" 2> "$log" &
    SERVER_PID=$!
    PORT=""
    local tries=0
    while [ -z "$PORT" ] && [ $tries -lt 100 ]; do
        sleep 0.05
        PORT=$(sed -n 's/.*listening on [^ ]*:\([0-9][0-9]*\) .*/\1/p' "$log")
        tries=$((tries + 1))
    done
    if [ -z "$PORT" ]; then
        echo "error: server did not start:" >&2
        cat "$log" >&2
        exit 1
    fi
}

# send <bytes>: one connection, one write, then QUIT; prints what was sent and every reply.
send() {
    local requests=$1
    printf '%s' "$requests" | sed 's/^/  > /'
    exec 3<>"/dev/tcp/127.0.0.1/$PORT"
    printf '%sQUIT\n' "$requests" >&3
    tr -d '\r' <&3 | sed 's/^/  < /'   # the server closes after BYE, so this ends
    exec 3<&- 3>&-
}

# ---------------------------------------------------------------------------------------------
step "1. Start a server that fsyncs every write before answering (--sync always)"
start_server "$WORK/server1.log" --data-dir "$WORK/data" --sync always
note "  $("$SERVER" --version)"
note "  listening on 127.0.0.1:$PORT, data in $WORK/data"

step "2. The seven commands"
send $'PING\nSET greeting hello, world\nGET greeting\nEXISTS greeting\nGET nope\nSET city Frankfurt am Main\nDBSIZE\nDEL city\nDBSIZE\n'

step "3. Errors are answered, and the connection stays usable"
send $'FLY away\nGET\nGET  greeting\nPING\n'

step "4. Pipelining: three requests in one write, three replies in order"
send $'SET a 1\nSET b 2\nGET a\n'

step "5. TCP is a byte stream: one request split across two writes"
exec 3<>"/dev/tcp/127.0.0.1/$PORT"
printf 'SET wo' >&3
note "  > SET wo             (first write: half a request; the server waits for the rest)"
sleep 1
printf 'rd stream\nGET word\nQUIT\n' >&3
note "  > rd stream          (second write, one second later: together \"SET word stream\")"
note "  > GET word"
tr -d '\r' <&3 | sed 's/^/  < /'
exec 3<&- 3>&-

step "6. Crash: kill -9, then restart on the same data"
{ kill -KILL "$SERVER_PID"; wait "$SERVER_PID"; } 2>/dev/null || true   # silence bash's "Killed" notice
note "  server killed with SIGKILL: no shutdown code ran"
start_server "$WORK/server2.log" --data-dir "$WORK/data" --sync always
grep -E "recovered|persistence:" "$WORK/server2.log" | sed 's/^/  log: /'
send $'GET greeting\nGET word\nDBSIZE\n'

step "7. Graceful shutdown on SIGTERM (Ctrl-C works the same)"
kill -TERM "$SERVER_PID"
status=0
wait "$SERVER_PID" || status=$?
SERVER_PID=""
grep -E "received|shutdown complete" "$WORK/server2.log" | sed 's/^/  log: /'
note "  exit status $status"

step "8. A short load test (in-memory server, 8 clients, 90 % GET, 2 seconds)"
start_server "$WORK/server3.log" --in-memory
"$LOADGEN" --port "$PORT" --clients 8 --duration 2 --warmup 1 --keys 1000 | sed 's/^/  /'
note "  ($( [ "$(basename "$BUILD")" = "bench" ] && echo "optimised build" || echo "debug build: unoptimised, see docs/benchmarks.md for real numbers"))"
kill -TERM "$SERVER_PID"
wait "$SERVER_PID" || true
SERVER_PID=""

printf '\n%sDone.%s Protocol: docs/protocol.md · Design: docs/architecture.md · Numbers: docs/benchmarks.md\n' "$BOLD" "$RESET"
