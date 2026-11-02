#pragma once

#include <string>

namespace cppkv {

// Formats a version number encoded as MAJOR * 100000 + MINOR * 100 + PATCH,
// the scheme used by both BOOST_VERSION and BOOST_ASIO_VERSION.
//   108300 -> "1.83.0", 103800 -> "1.38.0"
[[nodiscard]] std::string format_version_number(unsigned long encoded);

// Boost version this binary was compiled against, e.g. "1.90.0".
[[nodiscard]] std::string boost_version();

// Asio version this binary was compiled against, e.g. "1.38.0".
[[nodiscard]] std::string asio_version();

// One-line summary printed by `cppkv-server --version`, e.g.
//   "cppkv-server 1.0.0 (Boost 1.90.0, Asio 1.38.0, C++202002)"
[[nodiscard]] std::string build_summary();

} // namespace cppkv