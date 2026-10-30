#pragma once

#include <boost/asio/ip/tcp.hpp>

#include <string>

namespace cppkv {

// "127.0.0.1:7379" for IPv4, "[::1]:7379" for IPv6 (brackets keep the port
// separable from the address's own colons).
[[nodiscard]] inline std::string format_endpoint(const boost::asio::ip::tcp::endpoint& endpoint) {
    const auto address = endpoint.address();
    std::string text = address.is_v6() ? "[" + address.to_string() + "]" : address.to_string();
    text += ':';
    text += std::to_string(endpoint.port());
    return text;
}

} // namespace cppkv
