# cppkv-loadgen: a TCP load generator for cppkv-server (docs/benchmarks.md).
#
# The logic lives in a library so the unit and integration tests can call it
# directly; the executable only parses the command line and prints.
# It talks to the server over the protocol only, so it does not link cppkv_core.

add_library(cppkv_loadgen STATIC
    latency_histogram.cpp
    load_generator.cpp
    load_options.cpp
)
target_include_directories(cppkv_loadgen PUBLIC "${PROJECT_SOURCE_DIR}/tools")   # "loadgen/..."
target_link_libraries(cppkv_loadgen PUBLIC Boost::headers Threads::Threads)
target_compile_definitions(cppkv_loadgen PUBLIC BOOST_ASIO_NO_DEPRECATED)
cppkv_set_warnings(cppkv_loadgen)

add_executable(cppkv-loadgen main.cpp)
target_link_libraries(cppkv-loadgen PRIVATE cppkv_loadgen)
cppkv_set_warnings(cppkv-loadgen)
