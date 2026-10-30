#include "cppkv/protocol/request_parser.hpp"

#include <array>
#include <optional>
#include <string>
#include <utility>

namespace cppkv {

namespace {

constexpr std::string_view kEmptyTokenMessage = "empty token (leading, trailing or repeated space)";

constexpr char ascii_upper(char c) noexcept {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// ASCII-only, locale-independent comparison. (std::toupper(char) is undefined
// behaviour for negative char values, i.e. bytes >= 0x80 where char is signed.)
bool equals_ignore_case(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_upper(a[i]) != ascii_upper(b[i])) {
            return false;
        }
    }
    return true;
}

std::optional<CommandType> lookup_command(std::string_view name) noexcept {
    constexpr std::array commands{CommandType::ping, CommandType::set,    CommandType::get,  CommandType::del,
                                  CommandType::exists, CommandType::dbsize, CommandType::quit};
    for (const CommandType command : commands) {
        if (equals_ignore_case(name, to_string(command))) {
            return command;
        }
    }
    return std::nullopt;
}

// Client text echoed in an error message: printable ASCII only, truncated.
std::string printable_excerpt(std::string_view text) {
    constexpr std::size_t kMaxEcho = 32;
    std::string out;
    for (const char c : text.substr(0, kMaxEcho)) {
        out += (c >= 0x20 && c <= 0x7E) ? c : '?';
    }
    if (text.size() > kMaxEcho) {
        out += "...";
    }
    return out;
}

ProtocolError error(ErrorCode code, std::string message) {
    return ProtocolError{code, std::move(message)};
}

ProtocolError wrong_args(CommandType command, std::string_view expectation) {
    return error(ErrorCode::wrong_args, "'" + std::string{to_string(command)} + "' " + std::string{expectation});
}

ProtocolError invalid_key() {
    return error(ErrorCode::invalid_key,
                 "key must be 1-" + std::to_string(kMaxKeyBytes) + " printable ASCII characters without spaces");
}

ParseResult parse_set(std::string_view arguments, bool has_arguments) {
    if (!has_arguments) {
        return wrong_args(CommandType::set, "takes a key and a value");
    }
    const std::size_t key_end = arguments.find(' ');
    if (key_end == std::string_view::npos) {
        return wrong_args(CommandType::set, "takes a key and a value");
    }
    const std::string_view key = arguments.substr(0, key_end);
    if (key.empty()) {
        return error(ErrorCode::protocol, std::string{kEmptyTokenMessage});
    }
    if (!is_valid_key(key)) {
        return invalid_key();
    }
    // Everything after the separator is the value, verbatim: it may be empty
    // and may contain spaces, including leading and trailing ones.
    return Request{CommandType::set, std::string{key}, std::string{arguments.substr(key_end + 1)}};
}

} // namespace

bool is_valid_key(std::string_view key) noexcept {
    if (key.empty() || key.size() > kMaxKeyBytes) {
        return false;
    }
    for (const char c : key) {
        // Compare as unsigned: on platforms where char is signed, bytes >= 0x80
        // are negative and would otherwise slip through a "c <= 0x7E" check.
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x21 || byte > 0x7E) {
            return false;
        }
    }
    return true;
}

ParseResult parse_request(std::string_view line) {
    if (line.find('\r') != std::string_view::npos) {
        return error(ErrorCode::protocol, "bare CR inside request");
    }

    const std::size_t space = line.find(' ');
    const std::string_view name = line.substr(0, space);
    if (name.empty()) {
        return error(ErrorCode::protocol, std::string{kEmptyTokenMessage});
    }

    const std::optional<CommandType> command = lookup_command(name);
    if (!command) {
        return error(ErrorCode::unknown_command, "unknown command '" + printable_excerpt(name) + "'");
    }

    const bool has_arguments = space != std::string_view::npos;
    const std::string_view arguments = has_arguments ? line.substr(space + 1) : std::string_view{};

    if (*command == CommandType::set) {
        return parse_set(arguments, has_arguments);
    }

    // All other commands take 0 or 1 argument. Count tokens, rejecting empty
    // ones; remember the first token as the key.
    std::size_t token_count = 0;
    std::string_view first_token;
    if (has_arguments) {
        std::string_view rest = arguments;
        for (;;) {
            const std::size_t next_space = rest.find(' ');
            const std::string_view token = rest.substr(0, next_space);
            if (token.empty()) {
                return error(ErrorCode::protocol, std::string{kEmptyTokenMessage});
            }
            if (token_count == 0) {
                first_token = token;
            }
            ++token_count;
            if (next_space == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(next_space + 1);
        }
    }

    switch (*command) {
    case CommandType::ping:
    case CommandType::dbsize:
    case CommandType::quit:
        if (token_count != 0) {
            return wrong_args(*command, "takes no arguments");
        }
        return Request{*command, {}, {}};

    case CommandType::get:
    case CommandType::del:
    case CommandType::exists:
        if (token_count != 1) {
            return wrong_args(*command, "takes exactly one argument: <key>");
        }
        if (!is_valid_key(first_token)) {
            return invalid_key();
        }
        return Request{*command, std::string{first_token}, {}};

    case CommandType::set:
        break;   // handled above
    }
    return error(ErrorCode::internal, "unhandled command");   // unreachable
}

} // namespace cppkv
