# cppkv_core: all CppKV components live in this static library.
# The server executable, tests and (later) benchmarks link against it, so each
# source file is compiled once and the same code is tested that is shipped.

# Generate cppkv/version.hpp from the project() version, so there is exactly
# one place (the top-level CMakeLists.txt) where the version number is set.
set(CPPKV_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    cppkv/common/version.hpp.in
    "${CPPKV_GENERATED_DIR}/cppkv/common/version.hpp"
    @ONLY)

add_library(cppkv_core STATIC
    cppkv/common/build_info.cpp
    cppkv/common/config.cpp
    cppkv/common/logger.cpp
    cppkv/common/process_stats.cpp
    cppkv/persistence/append_log.cpp
    cppkv/persistence/log_format.cpp
    cppkv/persistence/posix_file.cpp
    cppkv/protocol/line_framer.cpp
    cppkv/protocol/protocol.cpp
    cppkv/protocol/request_parser.cpp
    cppkv/protocol/response_encoder.cpp
    cppkv/server/client_session.cpp
    cppkv/server/command_dispatcher.cpp
    cppkv/server/connection_handler.cpp
    cppkv/server/session_registry.cpp
    cppkv/server/tcp_server.cpp
    cppkv/store/kv_store.cpp
)
add_library(CppKV::core ALIAS cppkv_core)

target_include_directories(cppkv_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"   # #include "cppkv/..."
        "${CPPKV_GENERATED_DIR}"        # #include "cppkv/common/version.hpp"
)

# PUBLIC: anything that links cppkv_core also needs Boost headers and threads,
# because cppkv headers will expose Asio types from Phase 3 onwards.
target_link_libraries(cppkv_core
    PUBLIC
        Boost::headers
        Threads::Threads
)

target_compile_definitions(cppkv_core
    PUBLIC
        # Asio: refuse to compile code that uses removed/deprecated APIs
        # (io_service, io_context::work, ...), on every Boost version.
        BOOST_ASIO_NO_DEPRECATED
)

cppkv_set_warnings(cppkv_core)

# ---------------------------------------------------------------------------
add_executable(cppkv-server main.cpp)
target_link_libraries(cppkv-server PRIVATE cppkv_core)
cppkv_set_warnings(cppkv-server)
