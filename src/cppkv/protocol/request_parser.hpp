#pragma once

#include "cppkv/protocol/protocol.hpp"

#include <string_view>

namespace cppkv {

// Turns one request line (terminator already removed, non-empty) into a
// validated Request, or a ProtocolError explaining why it was rejected.
// Pure function: no I/O, no store access, never throws except std::bad_alloc.
//
// Grammar (docs/protocol.md):
//   PING | DBSIZE | QUIT
//   GET <key> | DEL <key> | EXISTS <key>
//   SET <key> <value>        value = all bytes after the space following <key>
// - Command names are ASCII case-insensitive; keys are case-sensitive.
// - Tokens are separated by exactly one space. Empty tokens (leading,
//   trailing or repeated spaces) are PROTOCOL errors — except inside a SET
//   value, which is taken verbatim.
// - A CR anywhere in the line is a PROTOCOL error.
// Checks are applied in this order: CR, empty command name, unknown command,
// empty tokens, argument count, key validity.
[[nodiscard]] ParseResult parse_request(std::string_view line);

// True if `key` is 1..kMaxKeyBytes bytes of printable ASCII without space.
[[nodiscard]] bool is_valid_key(std::string_view key) noexcept;

} // namespace cppkv
