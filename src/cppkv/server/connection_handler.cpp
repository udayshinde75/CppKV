#include "cppkv/server/connection_handler.hpp"

#include "cppkv/protocol/request_parser.hpp"
#include "cppkv/protocol/response_encoder.hpp"

#include <string>
#include <utility>
#include <variant>

namespace cppkv {

ConnectionHandler::ConnectionHandler(CommandDispatcher& dispatcher, std::size_t max_line_bytes)
    : dispatcher_{dispatcher}, framer_{max_line_bytes}, max_line_bytes_{max_line_bytes} {}

void ConnectionHandler::feed(std::string_view bytes) {
    if (close_reason_.empty()) {
        framer_.feed(bytes);
    }
}

ConnectionHandler::Progress ConnectionHandler::process(std::string& out, std::size_t output_soft_limit) {
    if (!close_reason_.empty()) {
        return Progress::close;   // nothing after QUIT / TOO_LARGE is executed
    }

    for (;;) {
        if (out.size() >= output_soft_limit) {
            return Progress::output_ready;
        }

        const LineFramer::Next next = framer_.next();
        switch (next.status) {
        case LineFramer::Status::incomplete:
            return Progress::need_input;

        case LineFramer::Status::too_large:
            encode_response(Response::failure(ErrorCode::too_large,
                                              "request exceeds " + std::to_string(max_line_bytes_) + " bytes"),
                            out);
            close_reason_ = "request too large";
            return Progress::close;

        case LineFramer::Status::line:
            break;
        }

        if (next.line.empty()) {
            continue;   // empty lines are ignored and get no response
        }

        // next.line is a view into the framer's buffer. It stays valid here
        // because feed() — the only call that invalidates it — is not called
        // until this function returns. parse_request copies what it keeps.
        ParseResult parsed = parse_request(next.line);
        Response response = std::holds_alternative<Request>(parsed)
                                ? dispatcher_.execute(std::get<Request>(std::move(parsed)))
                                : Response::failure(std::get<ProtocolError>(std::move(parsed)));

        encode_response(response, out);
        ++requests_handled_;
        if (response.closes_connection()) {
            close_reason_ = response.kind == Response::Kind::bye ? "client quit" : "error closes connection";
            return Progress::close;
        }
    }
}

} // namespace cppkv
