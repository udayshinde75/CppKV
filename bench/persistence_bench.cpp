// Persistence micro-benchmarks: record encoding, CRC-32, and the cost of one
// logged mutation under each sync policy.
//
// The log is written to a fresh directory under the system temporary directory
// (TMPDIR on macOS and Linux). Results depend entirely on that file system and
// device: a tmpfs makes fsync free, a network or virtual disk makes it slow.
// Point TMPDIR at the disk you want to measure, and record which one it was.

#include "bench_util.hpp"

#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/persistence/append_log.hpp"
#include "cppkv/persistence/log_format.hpp"
#include "cppkv/store/kv_store.hpp"

#include "support/temp_dir.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace {

void BM_Crc32(benchmark::State& state) {
    const std::string bytes(static_cast<std::size_t>(state.range(0)), 'c');
    for (auto _ : state) {
        std::uint32_t crc = cppkv::log_format::crc32(bytes);
        benchmark::DoNotOptimize(crc);
    }
    state.SetBytesProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Crc32)->ArgName("bytes")->Arg(64)->Arg(1'024)->Arg(65'536);

// Building one SET record: header, payload and CRC.
void BM_EncodeSetRecord(benchmark::State& state) {
    const std::string key = cppkv::bench::key_for(1234);
    const std::string value(static_cast<std::size_t>(state.range(0)), 'v');
    for (auto _ : state) {
        std::string record = cppkv::log_format::encode_set(key, value);
        benchmark::DoNotOptimize(record);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EncodeSetRecord)->ArgName("value_bytes")->Arg(16)->Arg(1'024);

// One AppendLog::append_set per iteration: encode + write(2), plus under
// --sync always an fsync (F_FULLFSYNC on macOS) before it returns. Real time,
// not CPU time: the process sleeps while the device flushes.
void BM_AppendLogSet(benchmark::State& state) {
    const auto policy = static_cast<cppkv::SyncPolicy>(state.range(0));
    const cppkv::test::TempDir dir;
    std::ostringstream sink;
    cppkv::Logger logger{sink, cppkv::LogLevel::error};
    cppkv::KvStore store;
    cppkv::LogOptions options;
    options.sync = policy;
    cppkv::RecoveryReport report;
    const std::unique_ptr<cppkv::AppendLog> log =
        cppkv::AppendLog::open(dir.path() / "bench.log", store, options, logger, report);

    const std::string key = cppkv::bench::key_for(1234);
    const std::string value(static_cast<std::size_t>(state.range(1)), 'v');
    for (auto _ : state) {
        if (!log->append_set(key, value)) {
            state.SkipWithError("append failed: " + log->failure_reason());
            break;
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.counters["syncs"] = static_cast<double>(log->syncs_performed());
    state.SetLabel(std::string{cppkv::to_string(policy)});
}
BENCHMARK(BM_AppendLogSet)
    ->ArgNames({"sync", "value_bytes"})
    ->Args({static_cast<long>(cppkv::SyncPolicy::never), 16})
    ->Args({static_cast<long>(cppkv::SyncPolicy::never), 1'024})
    ->Args({static_cast<long>(cppkv::SyncPolicy::interval), 16})
    ->Args({static_cast<long>(cppkv::SyncPolicy::always), 16})
    ->Args({static_cast<long>(cppkv::SyncPolicy::always), 1'024})
    ->UseRealTime()
    ->Unit(benchmark::kMicrosecond);

} // namespace
