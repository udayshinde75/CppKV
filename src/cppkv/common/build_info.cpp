#include "cppkv/common/build_info.hpp"

#include "cppkv/common/version.hpp"

#include <boost/asio/version.hpp>
#include <boost/version.hpp>

namespace cppkv {

std::string format_version_number(unsigned long encoded) {
    const unsigned long major = encoded / 100000;
    const unsigned long minor = encoded / 100 % 1000;
    const unsigned long patch = encoded % 100;
    return std::to_string(major) + '.' + std::to_string(minor) + '.' + std::to_string(patch);
}

std::string boost_version() {
    return format_version_number(BOOST_VERSION);
}

std::string asio_version() {
    return format_version_number(BOOST_ASIO_VERSION);
}

std::string build_summary() {
    // __cplusplus is 202002L for C++20.
    return std::string{"cppkv-server "} + kProjectVersion + " (Boost " + boost_version() +
           ", Asio " + asio_version() + ", C++" + std::to_string(__cplusplus) + ")";
}

} // namespace cppkv
