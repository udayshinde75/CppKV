#include "loadgen/load_options.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <initializer_list>
#include <string_view>
#include <vector>

using cppkv::loadgen::LoadAction;
using cppkv::loadgen::LoadParseResult;
using cppkv::loadgen::OutputFormat;

namespace {

LoadParseResult parse(std::initializer_list<std::string_view> args) {
    const std::vector<std::string_view> list(args);
    return cppkv::loadgen::parse_load_options(list);
}

TEST(LoadOptions, DefaultsAreAClosedLoopRun) {
    const LoadParseResult r = parse({});
    ASSERT_EQ(r.action, LoadAction::run);
    EXPECT_EQ(r.options.host, "127.0.0.1");
    EXPECT_EQ(r.options.port, 7379);
    EXPECT_EQ(r.options.clients, 4U);
    EXPECT_EQ(r.options.pipeline, 1U);
    EXPECT_FALSE(cppkv::loadgen::is_open_loop(r.options));
    EXPECT_EQ(r.options.duration, std::chrono::seconds{10});
    EXPECT_EQ(r.options.warmup, std::chrono::seconds{2});
    EXPECT_EQ(r.options.read_percent, 90U);
    EXPECT_TRUE(r.options.populate);
}

TEST(LoadOptions, ParsesEveryOption) {
    const LoadParseResult r =
        parse({"--host", "localhost", "--port", "7400", "--clients", "16", "--rate", "50000", "--duration", "30",
               "--warmup", "0", "--keys", "1000", "--value-bytes", "1024", "--reads", "50", "--no-populate", "--seed",
               "7", "--format", "csv", "--label", "sync=always"});
    ASSERT_EQ(r.action, LoadAction::run) << r.error;
    EXPECT_EQ(r.options.host, "localhost");
    EXPECT_EQ(r.options.port, 7400);
    EXPECT_EQ(r.options.clients, 16U);
    EXPECT_EQ(r.options.rate, 50'000.0);
    EXPECT_TRUE(cppkv::loadgen::is_open_loop(r.options));
    EXPECT_EQ(r.options.duration, std::chrono::seconds{30});
    EXPECT_EQ(r.options.warmup, std::chrono::seconds{0});
    EXPECT_EQ(r.options.keys, 1'000U);
    EXPECT_EQ(r.options.value_bytes, 1'024U);
    EXPECT_EQ(r.options.read_percent, 50U);
    EXPECT_FALSE(r.options.populate);
    EXPECT_EQ(r.options.seed, 7U);
    EXPECT_EQ(r.options.format, OutputFormat::csv);
    EXPECT_EQ(r.options.label, "sync=always");
}

TEST(LoadOptions, RejectsInvalidInput) {
    for (const auto& args : std::vector<std::vector<std::string_view>>{
             {"--clients", "0"},
             {"--clients", "1025"},
             {"--pipeline", "-1"},
             {"--reads", "101"},
             {"--port", "0"},
             {"--port", "70000"},
             {"--duration", "0"},
             {"--value-bytes", "60001"},
             {"--rate", "1.5"},
             {"--format", "json"},
             {"--label", "a,b"},
             {"--keys"},
             {"--frobnicate", "1"},
             {"stray"},
         }) {
        const LoadParseResult r = cppkv::loadgen::parse_load_options(args);
        EXPECT_EQ(r.action, LoadAction::usage_error) << args.front();
        EXPECT_FALSE(r.error.empty());
    }
}

TEST(LoadOptions, PipelineAndRateContradictEachOther) {
    const LoadParseResult r = parse({"--rate", "1000", "--pipeline", "8"});
    ASSERT_EQ(r.action, LoadAction::usage_error);
    EXPECT_NE(r.error.find("closed loop only"), std::string::npos) << r.error;
}

TEST(LoadOptions, HelpWinsOverEverythingElse) {
    EXPECT_EQ(parse({"--clients", "3", "--help"}).action, LoadAction::help);
}

} // namespace
