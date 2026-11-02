#include "loadgen/load_options.hpp"

#include <charconv>
#include <limits>
#include <system_error>

namespace cppkv::loadgen {

namespace {

// Strict: digits only, whole string, within [min, max] (as the server's parser).
template <typename Unsigned>
bool parse_unsigned(std::string_view text, Unsigned min, Unsigned max, Unsigned& out) {
    Unsigned value{};
    const char* const first = text.data();
    const char* const last = first + text.size();
    const auto [end, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || end != last || value < min || value > max) {
        return false;
    }
    out = value;
    return true;
}

bool parse_rate(std::string_view text, double& out) {
    // std::from_chars for double is missing in older libc++ (Apple Clang);
    // a rate is a whole number of requests per second anyway.
    std::uint64_t value = 0;
    if (!parse_unsigned<std::uint64_t>(text, 0, static_cast<std::uint64_t>(kMaxRate), value)) {
        return false;
    }
    out = static_cast<double>(value);
    return true;
}

LoadParseResult usage_error(std::string message) {
    LoadParseResult result;
    result.action = LoadAction::usage_error;
    result.error = std::move(message);
    return result;
}

std::string invalid(std::string_view option, std::string_view value, std::string_view expected) {
    return "invalid value '" + std::string{value} + "' for " + std::string{option} + " (expected " +
           std::string{expected} + ")";
}

} // namespace

bool is_open_loop(const LoadOptions& options) noexcept {
    return options.rate > 0;
}

LoadParseResult parse_load_options(std::span<const std::string_view> args) {
    LoadParseResult result;
    LoadOptions& o = result.options;
    bool pipeline_given = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            result.action = LoadAction::help;
            return result;
        }
        if (arg == "--no-populate") {
            o.populate = false;
            continue;
        }
        if (i + 1 >= args.size()) {
            return usage_error(arg.starts_with("--") ? "option '" + std::string{arg} + "' requires a value"
                                                     : "unexpected argument '" + std::string{arg} + "'");
        }
        const std::string_view value = args[++i];
        unsigned seconds = 0;

        if (arg == "--host") {
            if (value.empty()) {
                return usage_error(invalid(arg, value, "a host name or address"));
            }
            o.host = std::string{value};
        } else if (arg == "--port") {
            if (!parse_unsigned<std::uint16_t>(value, 1, std::numeric_limits<std::uint16_t>::max(), o.port)) {
                return usage_error(invalid(arg, value, "1-65535"));
            }
        } else if (arg == "--clients") {
            if (!parse_unsigned<unsigned>(value, 1, kMaxClients, o.clients)) {
                return usage_error(invalid(arg, value, "1-" + std::to_string(kMaxClients)));
            }
        } else if (arg == "--pipeline") {
            if (!parse_unsigned<unsigned>(value, 1, kMaxPipeline, o.pipeline)) {
                return usage_error(invalid(arg, value, "1-" + std::to_string(kMaxPipeline)));
            }
            pipeline_given = true;
        } else if (arg == "--rate") {
            if (!parse_rate(value, o.rate)) {
                return usage_error(invalid(arg, value, "requests per second, 0-10000000; 0 = closed loop"));
            }
        } else if (arg == "--duration") {
            if (!parse_unsigned<unsigned>(value, 1, 3'600, seconds)) {
                return usage_error(invalid(arg, value, "1-3600 seconds"));
            }
            o.duration = std::chrono::seconds{seconds};
        } else if (arg == "--warmup") {
            if (!parse_unsigned<unsigned>(value, 0, 3'600, seconds)) {
                return usage_error(invalid(arg, value, "0-3600 seconds"));
            }
            o.warmup = std::chrono::seconds{seconds};
        } else if (arg == "--keys") {
            if (!parse_unsigned<std::size_t>(value, 1, kMaxKeys, o.keys)) {
                return usage_error(invalid(arg, value, "1-" + std::to_string(kMaxKeys)));
            }
        } else if (arg == "--value-bytes") {
            if (!parse_unsigned<std::size_t>(value, 0, kMaxValueBytes, o.value_bytes)) {
                return usage_error(invalid(arg, value, "0-" + std::to_string(kMaxValueBytes)));
            }
        } else if (arg == "--reads") {
            if (!parse_unsigned<unsigned>(value, 0, 100, o.read_percent)) {
                return usage_error(invalid(arg, value, "a GET percentage, 0-100"));
            }
        } else if (arg == "--seed") {
            if (!parse_unsigned<std::uint64_t>(value, 0, std::numeric_limits<std::uint64_t>::max(), o.seed)) {
                return usage_error(invalid(arg, value, "an unsigned integer"));
            }
        } else if (arg == "--format") {
            if (value == "text") {
                o.format = OutputFormat::text;
            } else if (value == "csv") {
                o.format = OutputFormat::csv;
            } else {
                return usage_error(invalid(arg, value, "text or csv"));
            }
        } else if (arg == "--label") {
            if (value.find_first_of(",\"\r\n") != std::string_view::npos) {
                return usage_error(invalid(arg, value, "text without commas, quotes or line breaks"));
            }
            o.label = std::string{value};
        } else {
            return usage_error("unknown option '" + std::string{arg} + "'");
        }
    }

    // In an open loop, requests are sent on a schedule regardless of replies,
    // so the number in flight is not a parameter; a pipeline depth would
    // contradict the schedule.
    if (is_open_loop(o) && pipeline_given && o.pipeline != 1) {
        return usage_error("--pipeline applies to the closed loop only; do not combine it with --rate");
    }
    return result;
}

} // namespace cppkv::loadgen
