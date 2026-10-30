#include "cppkv/server/command_dispatcher.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace cppkv {

namespace {

Response boolean(bool value) {
    return Response::integer_value(value ? 1U : 0U);
}

} // namespace

Response CommandDispatcher::execute(Request request) {
    switch (request.command) {
    case CommandType::ping:
        return Response::pong();
    case CommandType::set:
        store_.set(std::move(request.key), std::move(request.value));
        return Response::ok();
    case CommandType::get: {
        std::optional<std::string> value = store_.get(request.key);
        return value ? Response::value(std::move(*value)) : Response::nil();
    }
    case CommandType::del:
        return boolean(store_.erase(request.key));
    case CommandType::exists:
        return boolean(store_.contains(request.key));
    case CommandType::dbsize:
        // size_t is uint64_t on 64-bit Linux, a distinct type of the same
        // width on macOS. The implicit conversion is lossless on every
        // supported platform; an explicit cast would be "useless" on Linux.
        static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
        return Response::integer_value(store_.size());
    case CommandType::quit:
        return Response::bye();
    }
    return Response::failure(ErrorCode::internal, "unhandled command");   // unreachable
}

} // namespace cppkv
