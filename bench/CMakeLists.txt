# ---------------------------------------------------------------------------
# Google Benchmark, pinned by full commit hash (tag v1.10.0).
#
# Unlike GoogleTest, Google Benchmark publishes no release tarball, and GitHub's
# generated source archives are not guaranteed to be byte-identical over time,
# so a SHA-256 of one could break. A Git commit hash identifies the exact
# source tree just as tamper-evidently. The cost: Git must be installed and the
# repository history is cloned once (GIT_SHALLOW needs a branch or tag name).
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        a6b4a9f00b36c19af9997d68a1cf6e0b5b3d032c   # v1.10.0
    SYSTEM
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)      # its own test suite
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "" FORCE)       # our -Werror is for our code only
FetchContent_MakeAvailable(googlebenchmark)

add_executable(cppkv_micro_bench
    persistence_bench.cpp
    protocol_bench.cpp
    server_bench.cpp
    store_bench.cpp
)
target_link_libraries(cppkv_micro_bench PRIVATE cppkv_core benchmark::benchmark_main)
# tests/support/temp_dir.hpp: the log benchmarks write to a temporary directory.
target_include_directories(cppkv_micro_bench PRIVATE "${PROJECT_SOURCE_DIR}/tests")
cppkv_set_warnings(cppkv_micro_bench)

# Smoke test: every benchmark runs once (one iteration), so a benchmark that
# crashes, or trips a sanitizer, fails `ctest`. This measures nothing.
add_test(NAME bench.smoke COMMAND cppkv_micro_bench --benchmark_min_time=1x)
set_tests_properties(bench.smoke PROPERTIES LABELS bench TIMEOUT 120)
