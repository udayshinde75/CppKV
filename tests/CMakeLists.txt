# ---------------------------------------------------------------------------
# GoogleTest, pinned by version and SHA-256.
#
# The URL is the official release asset. URL_HASH makes the download
# reproducible and tamper-evident: CMake refuses an archive with another hash.
# SYSTEM (CMake 3.25+) marks GoogleTest's headers as system headers, so our
# strict warning flags do not report warnings inside GoogleTest.
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(googletest
    URL      https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)    # not needed yet
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)  # never install test frameworks
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# ---------------------------------------------------------------------------
# Unit tests: one executable, one source file per component under test.
# ---------------------------------------------------------------------------
add_executable(cppkv_unit_tests
    unit/asio_smoke_test.cpp
    unit/build_info_test.cpp
    unit/command_dispatcher_test.cpp
    unit/config_test.cpp
    unit/connection_handler_test.cpp
    unit/kv_store_test.cpp
    unit/line_framer_test.cpp
    unit/logger_test.cpp
    unit/request_parser_test.cpp
    unit/response_encoder_test.cpp
)
target_link_libraries(cppkv_unit_tests PRIVATE cppkv_core GTest::gtest_main)
cppkv_set_warnings(cppkv_unit_tests)

# PRE_TEST: list the tests when CTest runs, not at build time. Building does not
# execute the test binary, which matters for sanitizer builds and cross-builds.
gtest_discover_tests(cppkv_unit_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit)

# ---------------------------------------------------------------------------
# Integration tests: real TCP connections to an in-process server.
# TIMEOUT turns a hang (e.g. a read waiting for a reply that never comes) into
# a test failure, even when ctest is run without a preset.
# ---------------------------------------------------------------------------
add_executable(cppkv_integration_tests
    integration/server_test.cpp
)
target_link_libraries(cppkv_integration_tests PRIVATE cppkv_core GTest::gtest_main)
cppkv_set_warnings(cppkv_integration_tests)

gtest_discover_tests(cppkv_integration_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS integration TIMEOUT 30)

# ---------------------------------------------------------------------------
# Executable tests: run the real binary and check its observable behaviour.
# ---------------------------------------------------------------------------
add_test(NAME cli.version COMMAND cppkv-server --version)
set_tests_properties(cli.version PROPERTIES
    PASS_REGULAR_EXPRESSION "^cppkv-server ${PROJECT_VERSION} \\(Boost [0-9]+\\.[0-9]+\\.[0-9]+, Asio [0-9]+\\.[0-9]+\\.[0-9]+, C\\+\\+[0-9]+\\)")

add_test(NAME cli.help COMMAND cppkv-server --help)
set_tests_properties(cli.help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: cppkv-server")

# An unknown flag must exit with status 2 (usage error) and explain why on
# stderr. WILL_FAIL would also "pass" on a crash, so a small script checks the
# exact exit code instead.
add_test(NAME cli.unknown_option
    COMMAND ${CMAKE_COMMAND}
        "-DCOMMAND=$<TARGET_FILE:cppkv-server>"
        "-DARGS=--no-such-flag"
        "-DEXPECTED_EXIT=2"
        "-DEXPECTED_STDERR=unknown option '--no-such-flag'"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAndCheck.cmake")

add_test(NAME cli.invalid_port
    COMMAND ${CMAKE_COMMAND}
        "-DCOMMAND=$<TARGET_FILE:cppkv-server>"
        "-DARGS=--port;65536"
        "-DEXPECTED_EXIT=2"
        "-DEXPECTED_STDERR=invalid port '65536'"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunAndCheck.cmake")
