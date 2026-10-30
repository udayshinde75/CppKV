#include "cppkv/protocol/response_encoder.hpp"

namespace cppkv {

void encode_response(const Response& response, std::string& out) {
    switch (response.kind) {
    case Response::Kind::pong:
        out += "PONG";
        break;
    case Response::Kind::ok:
        out += "OK";
        break;
    case Response::Kind::nil:
        out += "NIL";
        break;
    case Response::Kind::bye:
        out += "BYE";
        break;
    case Response::Kind::integer:
        out += "INT ";
        out += std::to_string(response.integer);
        break;
    case Response::Kind::value:
        out += "VALUE ";
        out += std::to_string(response.payload.size());
        out += ' ';
        out += response.payload;
        break;
    case Response::Kind::error:
        out += "ERR ";
        out += to_string(response.error);
        out += ' ';
        out += response.payload;
        break;
    }
    out += "\r\n";
}

} // namespace cppkv
