// Server-path micro-benchmarks, still without sockets:
// - the whole per-request CPU path (framing, parsing, dispatch, encoding) as
//   one session runs it, which bounds what one I/O thread can serve;
// - the single store mutex under contention from several threads (ADR 0002:
//   "one std::mutex is the simplest correct choice; Phase 8 measures").

#include "bench_util.hpp"

#include "cppkv/common/logger.hpp"
#include "cppkv/persistence/append_log.hpp"
#include "cppkv/protocol/protocol.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/connection_handler.hpp"
#include "cppkv/store/kv_store.hpp"

#include "support/temp_dir.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kLineLimit = 65'536;
constexpr std::size_t kOutputSoftLimit = 64 * 1024;   // as ClientSession uses
constexpr std::size_t kKeys = 10'000;

// 100 requests, 90 % GET / 10 % SET of 16-byte values on random existing keys,
// as one pipelined read would deliver them.
std::string make_burst() {
    std::mt19937_64 rng{7};
    std::string burst;
    for (int i = 0; i < 100; ++i) {
        const std::string key = cppkv::bench::key_for(rng() % kKeys);
        burst += (rng() % 100 < 90) ? "GET " + key + "\n" : "SET " + key + " " + std::string(16, 'w') + "\n";
    }
    return burst;
}

void fill(cppkv::KvStore& store) {
    for (std::size_t i = 0; i < kKeys; ++i) {
        store.set(cppkv::bench::key_for(i), std::string(16, 'v'));
    }
}

void run_handler(benchmark::State& state, cppkv::CommandDispatcher& dispatcher) {
    const std::string burst = make_burst();
    cppkv::ConnectionHandler handler{dispatcher, kLineLimit};
    std::string out;
    for (auto _ : state) {
        handler.feed(burst);
        while (handler.process(out, kOutputSoftLimit) == cppkv::ConnectionHandler::Progress::output_ready) {
            out.clear();
        }
        out.clear();   // the session would write it to the socket here
    }
    state.SetItemsProcessed(state.iterations() * 100);
}

// In-memory server (--in-memory): no log.
void BM_HandlerPipelineInMemory(benchmark::State& state) {
    cppkv::KvStore store;
    fill(store);
    cppkv::CommandDispatcher dispatcher{store};
    run_handler(state, dispatcher);
}
BENCHMARK(BM_HandlerPipelineInMemory);

// The same burst with the log and --sync never: each SET adds a write(2).
void BM_HandlerPipelineLogNoSync(benchmark::State& state) {
    const cppkv::test::TempDir dir;
    std::ostringstream sink;
    cppkv::Logger logger{sink, cppkv::LogLevel::error};
    cppkv::KvStore store;
    cppkv::LogOptions options;
    options.sync = cppkv::SyncPolicy::never;
    cppkv::RecoveryReport report;
    const std::unique_ptr<cppkv::AppendLog> log =
        cppkv::AppendLog::open(dir.path() / "bench.log", store, options, logger, report);
    fill(store);   // directly, not logged: only the measured SETs are
    cppkv::CommandDispatcher dispatcher{store, log.get()};
    run_handler(state, dispatcher);
}
BENCHMARK(BM_HandlerPipelineLogNoSync);

// N threads calling one dispatcher (90 % GET / 10 % SET). Reported items/s is
// the total over all threads, measured in real time. With a perfectly
// parallel store it would grow with the thread count; under one mutex it
// cannot exceed the single-thread rate, and usually falls as threads contend.
std::optional<cppkv::KvStore> g_store;
std::optional<cppkv::CommandDispatcher> g_dispatcher;

void BM_DispatcherContended(benchmark::State& state) {
    if (state.thread_index() == 0) {
        g_store.emplace();
        fill(*g_store);
        g_dispatcher.emplace(*g_store);
    }
    // Google Benchmark starts every thread's loop together, after the setup above.
    std::mt19937_64 rng{static_cast<std::uint64_t>(state.thread_index()) + 1};
    const std::string value(16, 'w');
    for (auto _ : state) {
        std::string key = cppkv::bench::key_for(rng() % kKeys);
        const bool is_get = rng() % 100 < 90;
        cppkv::Request request{is_get ? cppkv::CommandType::get : cppkv::CommandType::set, std::move(key),
                               is_get ? std::string{} : value};
        cppkv::Response response = g_dispatcher->execute(std::move(request));
        benchmark::DoNotOptimize(response);
    }
    state.SetItemsProcessed(state.iterations());
    if (state.thread_index() == 0) {
        // Every thread has left its loop before thread 0 gets here.
        g_dispatcher.reset();
        g_store.reset();
    }
}
BENCHMARK(BM_DispatcherContended)->ThreadRange(1, 8)->UseRealTime();

} // namespace
