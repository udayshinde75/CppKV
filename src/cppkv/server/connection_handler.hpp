#pragma once

#include "cppkv/protocol/line_framer.hpp"
#include "cppkv/server/command_dispatcher.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cppkv {

// The complete protocol logic of one connection, without any I/O
// ("sans-I/O" design): bytes in, response bytes out.
//
//   feed(bytes)  ->  LineFramer -> parse_request -> CommandDispatcher
//                ->  encode_response -> appended to `out` by process()
//
// The caller (ClientSession) only moves bytes between the socket and this
// object. Because no socket is involved, every protocol behaviour — including
// requests split at every possible byte position — is unit-testable
// deterministically, and Phase 5 can change the I/O model without touching
// the protocol.
//
// Bounded output: process() stops once `out` reaches the caller's soft limit,
// leaving remaining requests buffered. Without this, 700 pipelined
// "GET k\n" (4 KiB of input) for a 64 KiB value would produce ~45 MB of
// output in one go. The caller writes `out` and calls process() again.
//
// Thread safety: none; one connection is driven by one thread at a time.
class ConnectionHandler {
public:
    enum class Progress {
        need_input,     // every complete request is answered; read more bytes
        output_ready,   // `out` reached the soft limit; write it, then call process() again
        close,          // write `out`, then close the connection
    };

    ConnectionHandler(CommandDispatcher& dispatcher, std::size_t max_line_bytes);

    void feed(std::string_view bytes);

    // Answers buffered requests in order, appending responses to `out`.
    Progress process(std::string& out, std::size_t output_soft_limit);

    // Bytes of an incomplete request still buffered (for disconnect logging).
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return framer_.buffered_bytes(); }

    // Non-empty request lines answered so far (for the connection's log line).
    [[nodiscard]] std::uint64_t requests_handled() const noexcept { return requests_handled_; }

    // Why process() returned close, e.g. "client quit". Empty before that.
    [[nodiscard]] std::string_view close_reason() const noexcept { return close_reason_; }

private:
    CommandDispatcher& dispatcher_;
    LineFramer framer_;
    std::size_t max_line_bytes_;
    std::string_view close_reason_;   // always points to a string literal
    std::uint64_t requests_handled_ = 0;
};

} // namespace cppkv
