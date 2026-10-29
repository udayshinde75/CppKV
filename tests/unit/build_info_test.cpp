#include "cppkv/common/build_info.hpp"
#include "cppkv/common/version.hpp"

#include <boost/version.hpp>
#include <gtest/gtest.h>

#include <string>

namespace {

TEST(FormatVersionNumber, DecodesBoostScheme) {
    EXPECT_EQ(cppkv::format_version_number(108300), "1.83.0");
    EXPECT_EQ(cppkv::format_version_number(109000), "1.90.0");
}

TEST(FormatVersionNumber, DecodesAsioScheme) {
    EXPECT_EQ(cppkv::format_version_number(103800), "1.38.0");
}

TEST(FormatVersionNumber, KeepsAllThreeComponents) {
    // Minor is three digits wide, patch two: 2.105.7 encodes as 210507.
    EXPECT_EQ(cppkv::format_version_number(210507), "2.105.7");
    EXPECT_EQ(cppkv::format_version_number(0), "0.0.0");
}

TEST(BuildInfo, CompiledAgainstSupportedBoost) {
    // CMakeLists.txt requires Boost >= 1.83; this guards the header we compile against.
    EXPECT_GE(BOOST_VERSION, 108300);
    EXPECT_EQ(cppkv::boost_version(), cppkv::format_version_number(BOOST_VERSION));
}

TEST(BuildInfo, SummaryStartsWithProgramNameAndVersion) {
    const std::string expected_prefix = std::string{"cppkv-server "} + cppkv::kProjectVersion + " (";
    EXPECT_EQ(cppkv::build_summary().rfind(expected_prefix, 0), 0U) << cppkv::build_summary();
}

} // namespace
