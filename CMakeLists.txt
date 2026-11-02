# CppKV — top-level build file.
#
# The version range tells CMake: "this project needs at least 3.25 and has been
# tested up to 4.4". Policies introduced up to 4.4 are set to NEW, so CMake 4
# does not emit compatibility warnings, while CMake 3.25–3.31 still work.
cmake_minimum_required(VERSION 3.25...4.4)

project(CppKV
    VERSION 1.0.0
    DESCRIPTION "Educational Redis-inspired key-value server"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# PROJECT_IS_TOP_LEVEL (CMake 3.21+) is false when CppKV is consumed via
# add_subdirectory/FetchContent; tests are then off by default.
option(CPPKV_BUILD_TESTS "Build the CppKV test suite" ${PROJECT_IS_TOP_LEVEL})
# Google Benchmark is fetched only when micro-benchmarks are requested (presets
# "bench" and "ci"), so ordinary builds need no extra download.
option(CPPKV_BUILD_BENCHMARKS "Build the Google Benchmark micro-benchmarks" OFF)
option(CPPKV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (used in CI)" OFF)
set(CPPKV_SANITIZERS "" CACHE STRING
    "Semicolon-separated sanitizers: e.g. 'address;undefined' or 'thread'")

# ---------------------------------------------------------------------------
# Language standard — applied to every target, including fetched dependencies,
# so that GoogleTest and CppKV are compiled with the same standard library ABI.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)   # -std=c++20, not -std=gnu++20

# Single-config generators (Ninja, Makefiles) default to an empty build type,
# which means "no optimisation and no debug info". Pick Debug instead.
get_property(cppkv_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT cppkv_is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)

# Sanitizers must be enabled before any target (including GoogleTest) is
# defined, because they are applied as directory-wide compile/link options.
cppkv_enable_sanitizers("${CPPKV_SANITIZERS}")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
# Boost: header-only usage (Asio). CONFIG mode uses BoostConfig.cmake shipped
# with Boost itself; the old FindBoost module is removed in CMake 3.30+.
# No COMPONENTS are requested: Boost.System is header-only since 1.69 and its
# stub library no longer exists in 1.89+.
find_package(Boost 1.83 CONFIG REQUIRED)
message(STATUS "CppKV: Boost ${Boost_VERSION} at ${Boost_DIR}")

# Asio uses POSIX threads internally on Linux.
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(src)
add_subdirectory(tools)   # cppkv-loadgen: needs nothing beyond Boost, always built

if(CPPKV_BUILD_TESTS OR CPPKV_BUILD_BENCHMARKS)
    enable_testing()
endif()
if(CPPKV_BUILD_TESTS)
    add_subdirectory(tests)
endif()
if(CPPKV_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
