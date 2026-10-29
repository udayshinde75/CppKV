// cppkv-server entry point.
//
// Phase 1: only --version and --help. The TCP server arrives in Phase 3.

#include "cppkv/common/build_info.hpp"

#include <cstddef>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string_view>

namespace {

// Exit codes. 2 for usage errors follows the common Unix convention.
constexpr int kExitOk = EXIT_SUCCESS;
constexpr int kExitFailure = EXIT_FAILURE;
constexpr int kExitUsage = 2;

void print_usage(std::ostream& out) {
    out << "Usage: cppkv-server [--version] [--help]\n"
           "\n"
           "  --version   Print version and build information, then exit\n"
           "  --help      Print this message, then exit\n";
}

int run(std::span<char* const> args) {
    // args[0] is the program name; options start at index 1.
    for (const std::string_view arg : args.subspan(1)) {
        if (arg == "--version") {
            std::cout << cppkv::build_summary() << '\n';
            return kExitOk;
        }
        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return kExitOk;
        }
        std::cerr << "cppkv-server: error: unknown option '" << arg << "'\n\n";
        print_usage(std::cout);
        return kExitUsage;
    }

    std::cout << cppkv::build_summary() << '\n'
              << "The network server is not implemented yet (planned for Phase 3).\n";
    return kExitOk;
}

} // namespace

int main(int argc, char* argv[]) {
    // An exception escaping main() calls std::terminate() without a useful
    // message, so catch everything here, report it, and fail cleanly.
    try {
        return run(std::span<char* const>{argv, static_cast<std::size_t>(argc)});
    } catch (const std::exception& e) {
        std::cerr << "cppkv-server: fatal: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "cppkv-server: fatal: unknown exception\n";
    }
    return kExitFailure;
}
