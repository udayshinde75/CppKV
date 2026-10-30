#pragma once

#include "cppkv/protocol/protocol.hpp"

#include <string>

namespace cppkv {

// Appends the wire form of `response` to `out` (docs/protocol.md §Responses):
//
//   PONG | OK | NIL | BYE
//   VALUE <len> <bytes>      <len> = exact byte length of <bytes> ("VALUE 0 " if empty)
//   INT <n>
//   ERR <CODE> <message>
//
// Every response ends with "\r\n". Appending (instead of returning a new
// string) lets the caller batch several responses into one socket write.
void encode_response(const Response& response, std::string& out);

} // namespace cppkv
