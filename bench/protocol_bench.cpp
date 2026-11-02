// Protocol micro-benchmarks: framing, parsing and encoding, the per-request
// work done outside the store lock.

#include "bench_util.hpp"

#include "cppkv/protocol/line_framer.hpp"
#include "cppkv/protocol/protocol.hpp"
#include "cppkv/protocol/request_parser.hpp"
#include "cppkv/protocol/response_encoder.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kLineLimit = 65'536;

void BM_ParseGet(benchmark::State& state) {
    const std::string line = "GET " + cppkv::bench::key_for(1234);
    for (auto _ : state) {
        cppkv::ParseResult result = cppkv::parse_request(line);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ParseGet);

// The parser validates every value byte (CR is an error) and copies the value
// into the Request, so the cost grows with the value size.
void BM_ParseSet(benchmark::State& state) {
    const std::string line =
        "SET " + cppkv::bench::key_for(1234) + " " + std::string(static_cast<std::size_t>(state.range(0)), 'v');
    for (auto _ : state) {
        cppkv::ParseResult result = cppkv::parse_request(line);
        benchmark::DoNotOptimize(result);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * static_cast<long>(line.size()));
}
BENCHMARK(BM_ParseSet)->ArgName("value_bytes")->Arg(16)->Arg(1'024)->Arg(16'384);

// Splitting a pipelined burst of 1000 GET lines, as one read delivers it.
void BM_FramePipelinedGets(benchmark::State& state) {
    std::string burst;
    constexpr std::size_t kLines = 1'000;
    for (std::size_t i = 0; i < kLines; ++i) {
        burst += "GET " + cppkv::bench::key_for(i) + "\n";
    }
    cppkv::LineFramer framer{kLineLimit};
    for (auto _ : state) {
        framer.feed(burst);
        std::size_t lines = 0;
        for (cppkv::LineFramer::Next next = framer.next(); next.status == cppkv::LineFramer::Status::line;
             next = framer.next()) {
            ++lines;
        }
        benchmark::DoNotOptimize(lines);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<long>(kLines));
    state.SetBytesProcessed(state.iterations() * static_cast<long>(burst.size()));
}
BENCHMARK(BM_FramePipelinedGets);

void BM_EncodeOk(benchmark::State& state) {
    const cppkv::Response response = cppkv::Response::ok();
    std::string out;
    for (auto _ : state) {
        out.clear();   // keeps its capacity, as the session's output buffer does
        cppkv::encode_response(response, out);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EncodeOk);

void BM_EncodeValue(benchmark::State& state) {
    const cppkv::Response response = cppkv::Response::value(std::string(static_cast<std::size_t>(state.range(0)), 'v'));
    std::string out;
    for (auto _ : state) {
        out.clear();
        cppkv::encode_response(response, out);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_EncodeValue)->ArgName("value_bytes")->Arg(16)->Arg(1'024)->Arg(16'384);

} // namespace
