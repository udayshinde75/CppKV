#include "cppkv/common/config.hpp"

#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>

#include <charconv>
#include <string>
#include <system_error>
#include <utility>

namespace cppkv {

namespace {

CliParseResult usage_error(std::string message) {
    CliParseResult result;
    result.action = CliAction::usage_error;
    result.error = std::move(message);
    return result;
}

// Strict port parsing: digits only, whole string consumed, 0..65535.
// std::from_chars never allocates, never throws and ignores the locale; it
// rejects "-1", "+80", " 80", and reports "65536" as out of range.
bool parse_port(std::string_view text, std::uint16_t& port) {
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [end, error] = std::from_chars(first, last, port);
    return error == std::errc{} && end == last;
}

} // namespace

CliParseResult parse_command_line(std::span<const std::string_view> args) {
    CliParseResult result;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];

        if (arg == "--version") {
            result.action = CliAction::print_version;
            return result;
        }
        if (arg == "--help" || arg == "-h") {
            result.action = CliAction::print_help;
            return result;
        }

        if (arg == "--port" || arg == "--bind") {
            if (i + 1 >= args.size()) {
                return usage_error("option '" + std::string{arg} + "' requires a value");
            }
            const std::string_view value = args[++i];

            if (arg == "--port") {
                if (!parse_port(value, result.config.port)) {
                    return usage_error("invalid port '" + std::string{value} + "' (expected 0-65535)");
                }
            } else {
                // Validate now so that a typo is a usage error, not a startup crash.
                boost::system::error_code ec;
                const auto address = boost::asio::ip::make_address(std::string{value}, ec);
                if (ec) {
                    return usage_error("invalid bind address '" + std::string{value} + "'");
                }
                result.config.bind_address = address.to_string();
            }
            continue;
        }

        return usage_error("unknown option '" + std::string{arg} + "'");
    }

    return result;
}

} // namespace cppkv
