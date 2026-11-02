#!/usr/bin/env bash
# Line and function coverage of the CppKV sources by the test suite (Linux, GCC + gcov).
#
#   scripts/coverage.sh            # summary table on stdout; details in build/coverage/coverage.json
#
# Builds Debug with --coverage into build/coverage, runs every test, then asks
# gcov (JSON output, GCC 9+) for the counts and summarises them with
# scripts/coverage_summary.py. Counts only src/ and tools/, not the tests or
# third-party code. Coverage says which code the tests EXECUTED, not whether they
# checked the results; the mutation checks recorded in PROGRESS.md cover that.
#
# Environment: CXX (default g++), GCOV (default gcov, must match the compiler),
# CMAKE_PREFIX_PATH as for any build.

set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build/coverage
CXX=${CXX:-g++}
GCOV=${GCOV:-gcov}

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_CXX_FLAGS="--coverage" -DCMAKE_EXE_LINKER_FLAGS="--coverage" -DCPPKV_BUILD_BENCHMARKS=OFF > /dev/null
cmake --build "$BUILD"

# Counters accumulate across runs in the .gcda files: start from zero.
find "$BUILD" -name '*.gcda' -delete
ctest --test-dir "$BUILD" --output-on-failure --no-tests=error > "$BUILD/ctest.log" 2>&1 || {
    tail -n 30 "$BUILD/ctest.log"
    echo "error: tests failed; coverage not reported" >&2
    exit 1
}
grep -E 'tests passed' "$BUILD/ctest.log"

# One JSON document per object file; the summary merges them (a header's lines
# appear in every object that includes it).
mkdir -p "$BUILD/gcov"
find "$BUILD/src" "$BUILD/tools" -name '*.gcda' -print0 |
    (cd "$BUILD/gcov" && xargs -0 "$GCOV" --json-format --stdout --preserve-paths > all.jsonl 2> gcov.stderr) ||
    { cat "$BUILD/gcov/gcov.stderr" >&2; exit 1; }
# The test executables also compile parts of src/ through headers; include them.
find "$BUILD/tests" -name '*.gcda' -print0 |
    (cd "$BUILD/gcov" && xargs -0 "$GCOV" --json-format --stdout --preserve-paths >> all.jsonl 2>> gcov.stderr)

python3 "$ROOT/scripts/coverage_summary.py" "$ROOT" "$BUILD/gcov/all.jsonl" "$BUILD/coverage.json"
