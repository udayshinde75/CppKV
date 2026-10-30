#include "cppkv/common/logger.hpp"

#include <gtest/gtest.h>

#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

TEST(Logger, FormatsTimestampLevelAndMessage) {
    std::ostringstream out;
    cppkv::Logger logger{out, cppkv::LogLevel::debug};

    logger.info("client connected peer=127.0.0.1:5000");

    const std::regex expected{
        R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z INFO  client connected peer=127\.0\.0\.1:5000\n$)"};
    EXPECT_TRUE(std::regex_match(out.str(), expected)) << out.str();
}

TEST(Logger, DropsMessagesBelowMinimumLevel) {
    std::ostringstream out;
    cppkv::Logger logger{out, cppkv::LogLevel::warn};

    logger.debug("d");
    logger.info("i");
    logger.warn("w");
    logger.error("e");

    const std::string text = out.str();
    EXPECT_EQ(text.find(" d\n"), std::string::npos);
    EXPECT_EQ(text.find(" i\n"), std::string::npos);
    EXPECT_NE(text.find("WARN  w\n"), std::string::npos);
    EXPECT_NE(text.find("ERROR e\n"), std::string::npos);
}

TEST(Logger, EnabledReflectsMinimumLevel) {
    std::ostringstream out;
    const cppkv::Logger logger{out, cppkv::LogLevel::info};
    EXPECT_FALSE(logger.enabled(cppkv::LogLevel::debug));
    EXPECT_TRUE(logger.enabled(cppkv::LogLevel::info));
    EXPECT_TRUE(logger.enabled(cppkv::LogLevel::error));
}

TEST(Logger, ConcurrentWritersProduceWholeLines) {
    // Without the mutex, characters from different threads could interleave
    // inside a line. Under TSan this test also proves there is no data race.
    std::ostringstream out;
    cppkv::Logger logger{out, cppkv::LogLevel::info};

    constexpr int kThreads = 4;
    constexpr int kLinesPerThread = 200;
    {
        std::vector<std::jthread> writers;
        for (int t = 0; t < kThreads; ++t) {
            writers.emplace_back([&logger, t] {
                const std::string message = "writer-" + std::to_string(t) + "-payload";
                for (int i = 0; i < kLinesPerThread; ++i) {
                    logger.info(message);
                }
            });
        }
    }   // jthreads join here

    std::istringstream lines{out.str()};
    const std::regex line_format{R"(^\S+ INFO  writer-\d-payload$)"};
    int count = 0;
    for (std::string line; std::getline(lines, line);) {
        ASSERT_TRUE(std::regex_match(line, line_format)) << "corrupted line: " << line;
        ++count;
    }
    EXPECT_EQ(count, kThreads * kLinesPerThread);
}

} // namespace
