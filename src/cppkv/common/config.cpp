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

// Strict unsigned parsing: digits only, whole string consumed, within
// [min, max]. std::from_chars never allocates, never throws and ignores the
// locale; it rejects "-1", "+80", " 80", and reports overflow as an error.
template <typename Unsigned>
bool parse_unsigned(std::string_view text, Unsigned min, Unsigned max, Unsigned& out) {
    Unsigned value{};
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [end, error] = std::from_chars(first, last, value);
    if (error != std::errc{} || end != last || value < min || value > max) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

CliParseResult parse_command_line(std::span<const std::string_view> args) {
    CliParseResult result;
    ServerConfig& config = result.config;

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

        if (arg != "--port" && arg != "--bind" && arg != "--threads" && arg != "--max-connections") {
            return usage_error("unknown option '" + std::string{arg} + "'");
        }
        if (i + 1 >= args.size()) {
            return usage_error("option '" + std::string{arg} + "' requires a value");
        }
        const std::string_view value = args[++i];

        if (arg == "--port") {
            if (!parse_unsigned<std::uint16_t>(value, 0, 65535, config.port)) {
                return usage_error("invalid port '" + std::string{value} + "' (expected 0-65535)");
            }
        } else if (arg == "--threads") {
            if (!parse_unsigned<std::size_t>(value, 1, kMaxIoThreads, config.io_threads)) {
                return usage_error("invalid thread count '" + std::string{value} + "' (expected 1-" +
                                   std::to_string(kMaxIoThreads) + ")");
            }
        } else if (arg == "--max-connections") {
            if (!parse_unsigned<std::size_t>(value, 1, kMaxConnectionsLimit, config.max_connections)) {
                return usage_error("invalid connection limit '" + std::string{value} + "' (expected 1-" +
                                   std::to_string(kMaxConnectionsLimit) + ")");
            }
        } else {   // --bind
            // Validate now so that a typo is a usage error, not a startup crash.
            boost::system::error_code ec;
            const auto address = boost::asio::ip::make_address(std::string{value}, ec);
            if (ec) {
                return usage_error("invalid bind address '" + std::string{value} + "'");
            }
            config.bind_address = address.to_string();
        }
    }

    return result;
}

} // namespace cppkv
