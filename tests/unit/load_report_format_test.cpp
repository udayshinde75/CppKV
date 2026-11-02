// The load generator's text and CSV output. scripts/bench_load.sh reads the CSV
// row by column NUMBER (`cut -d, -f11` and so on), so a reordered or inserted
// column would silently put wrong numbers into docs/benchmarks.md. These tests
// pin that contract.

#include "loadgen/load_generator.hpp"
#include "loadgen/load_options.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

using cppkv::loadgen::LoadOptions;
using cppkv::loadgen::LoadReport;

namespace {

std::vector<std::string> split_csv(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream{line};
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();   // a trailing empty field
    }
    return fields;
}

LoadReport sample_report() {
    LoadReport r;
    r.requests = 2'000;
    r.gets = 1'800;
    r.sets = 200;
    r.get_misses = 3;
    r.errors = 0;
    r.window_seconds = 2.0;
    r.latency.record(1'000);   // 1.0 us
    r.latency.record(3'000);   // 3.0 us
    return r;
}

TEST(LoadReportFormat, CsvHeaderAndRowHaveTheSameNumberOfColumns) {
    LoadOptions o;
    o.label = "e1/run1";
    const auto header = split_csv(cppkv::loadgen::csv_header());
    const auto row = split_csv(cppkv::loadgen::format_csv_row(o, sample_report()));
    EXPECT_EQ(header.size(), row.size());
    EXPECT_EQ(header.size(), 22U);
}

TEST(LoadReportFormat, ColumnsReadByBenchLoadShAreWhereItExpectsThem) {
    // 1-based column numbers used in scripts/bench_load.sh: throughput -f11,
    // p50 -f12, p99 -f14 (and docs/benchmarks.md is built from them).
    const auto header = split_csv(cppkv::loadgen::csv_header());
    ASSERT_GE(header.size(), 14U);
    EXPECT_EQ(header[0], "label");
    EXPECT_EQ(header[10], "throughput_rps");
    EXPECT_EQ(header[11], "p50_us");
    EXPECT_EQ(header[13], "p99_us");
}

TEST(LoadReportFormat, CsvRowCarriesTheMeasuredValues) {
    LoadOptions o;
    o.label = "e2/run7";
    o.clients = 16;
    o.pipeline = 4;
    o.read_percent = 50;
    o.keys = 1'000;
    o.value_bytes = 32;
    const auto row = split_csv(cppkv::loadgen::format_csv_row(o, sample_report()));
    ASSERT_EQ(row.size(), 22U);
    EXPECT_EQ(row[0], "e2/run7");
    EXPECT_EQ(row[1], "closed");
    EXPECT_EQ(row[2], "16");
    EXPECT_EQ(row[3], "4");
    EXPECT_EQ(row[5], "50");
    EXPECT_EQ(row[6], "1000");
    EXPECT_EQ(row[7], "32");
    EXPECT_EQ(row[9], "2000");    // requests
    EXPECT_EQ(row[10], "1000");   // 2000 requests in 2 s
    EXPECT_EQ(row[11], "1.0");    // p50: the 1 us sample (its bucket ends at 1.003 us)
    EXPECT_EQ(row[13], "3.0");    // p99: the 3 us sample, capped at the exact maximum
    EXPECT_EQ(row[15], "3.0");    // max
    EXPECT_EQ(row[16], "2.0");    // mean
    EXPECT_EQ(row[19], "3");      // GET misses
    EXPECT_EQ(row[20], "0");      // errors
    EXPECT_EQ(row[21], "0");      // not failed
}

TEST(LoadReportFormat, FailedRunIsMarkedInTheCsv) {
    LoadReport r = sample_report();
    r.failure = "connection reset";
    const auto row = split_csv(cppkv::loadgen::format_csv_row(LoadOptions{}, r));
    EXPECT_EQ(row.back(), "1");
}

TEST(LoadReportFormat, TextReportOfAClosedLoopRun) {
    const std::string text = cppkv::loadgen::format_text(LoadOptions{}, sample_report());
    EXPECT_NE(text.find("closed loop, 4 clients x pipeline 1"), std::string::npos) << text;
    EXPECT_NE(text.find("throughput: 1000 req/s"), std::string::npos) << text;
    EXPECT_NE(text.find("p50 1.0"), std::string::npos) << text;
    EXPECT_EQ(text.find("send lag"), std::string::npos) << "send lag exists only in the open loop";
    EXPECT_EQ(text.find("warning"), std::string::npos) << text;
    EXPECT_EQ(text.find("FAILED"), std::string::npos) << text;
}

TEST(LoadReportFormat, TextReportWarnsWhenTheOpenLoopMissedItsRate) {
    LoadOptions o;
    o.rate = 2'000;   // per second; the sample achieved 1000
    LoadReport r = sample_report();
    r.failure = "replies still missing";
    const std::string text = cppkv::loadgen::format_text(o, r);
    EXPECT_NE(text.find("open loop at 2000 req/s"), std::string::npos) << text;
    EXPECT_NE(text.find("send lag us"), std::string::npos) << text;
    EXPECT_NE(text.find("below the target rate"), std::string::npos) << text;
    EXPECT_NE(text.find("FAILED:     replies still missing"), std::string::npos) << text;
}

} // namespace
