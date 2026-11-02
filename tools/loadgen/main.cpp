// cppkv-loadgen: drives a running cppkv-server over TCP and reports throughput
// and latency percentiles. See docs/benchmarks.md for how it is used.

#include "loadgen/load_generator.hpp"
#include "loadgen/load_options.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

namespace {

void print_usage(std::ostream& out) {
    out << "Usage: cppkv-loadgen [options]\n"
           "\n"
           "Target\n"
           "  --host <name>        Server host (default 127.0.0.1)\n"
           "  --port <port>        Server port (default 7379)\n"
           "Load\n"
           "  --clients <n>        TCP connections, 1-1024 (default 4)\n"
           "  --pipeline <n>       Closed loop: requests in flight per connection, 1-1024 (default 1)\n"
           "  --rate <n>           Open loop: total requests/s on a fixed schedule; 0 = closed loop (default 0)\n"
           "  --duration <s>       Measured window in seconds (default 10)\n"
           "  --warmup <s>         Unmeasured load before the window (default 2)\n"
           "Workload\n"
           "  --reads <percent>    Share of GETs, 0-100; the rest are SETs (default 90)\n"
           "  --keys <n>           Key space, chosen uniformly (default 100000)\n"
           "  --value-bytes <n>    SET value size, 0-60000 (default 16)\n"
           "  --no-populate        Do not SET every key before the run (GETs may then miss)\n"
           "  --seed <n>           Request sequence seed (default 1)\n"
           "Output\n"
           "  --format text|csv    csv prints a header line and one result row (default text)\n"
           "  --label <text>       Free text for the csv 'label' column\n"
           "\n"
           "Latency is measured per request: from sending it (closed loop) or from its scheduled\n"
           "send time (open loop) to its reply. Exit status: 0 on success, 1 if the run failed or\n"
           "a reply was an error, 2 on a usage error.\n";
}

int run(std::span<char* const> argv) {
    const auto arguments = argv.size() > 1 ? argv.subspan(1) : std::span<char* const>{};
    const std::vector<std::string_view> args(arguments.begin(), arguments.end());
    const cppkv::loadgen::LoadParseResult parsed = cppkv::loadgen::parse_load_options(args);
    switch (parsed.action) {
    case cppkv::loadgen::LoadAction::help:
        print_usage(std::cout);
        return EXIT_SUCCESS;
    case cppkv::loadgen::LoadAction::usage_error:
        std::cerr << "cppkv-loadgen: error: " << parsed.error << "\n\n";
        print_usage(std::cerr);
        return 2;
    case cppkv::loadgen::LoadAction::run:
        break;
    }

    const cppkv::loadgen::LoadOptions& options = parsed.options;
    const cppkv::loadgen::LoadReport report = cppkv::loadgen::run_load(options);
    if (options.format == cppkv::loadgen::OutputFormat::csv) {
        std::cout << cppkv::loadgen::csv_header() << '\n' << cppkv::loadgen::format_csv_row(options, report) << '\n';
        if (!report.failure.empty()) {
            std::cerr << "cppkv-loadgen: run failed: " << report.failure << '\n';
        }
    } else {
        std::cout << cppkv::loadgen::format_text(options, report);
    }
    return report.failure.empty() && report.errors == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        return run(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& e) {
        std::cerr << "cppkv-loadgen: fatal: " << e.what() << '\n';
    }
    return EXIT_FAILURE;
}
