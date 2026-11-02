// KvStore micro-benchmarks: the in-memory part of every command, without the
// dispatcher's lock, parsing or I/O.
//
// Questions answered (docs/benchmarks.md):
// - What does a lookup cost, and how does that grow with the number of keys
//   (cache misses once the table no longer fits in the CPU caches)?
// - What does GET's copy of the value cost (architecture.md §6: "get returns a
//   copy; measure before optimising")? Compare StoreGetHit with StoreContains.

#include "bench_util.hpp"

#include "cppkv/store/kv_store.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace {

using cppkv::KvStore;
using cppkv::bench::make_keys;
using cppkv::bench::shuffled_indices;

struct FilledStore {
    FilledStore(std::size_t key_count, std::size_t value_bytes)
        : keys{make_keys(key_count)}, order{shuffled_indices(key_count)} {
        const std::string value(value_bytes, 'v');
        for (const std::string& key : keys) {
            store.set(key, value);
        }
    }
    KvStore store;
    std::vector<std::string> keys;
    std::vector<std::size_t> order;
};

// Arguments: {number of keys, value size in bytes}.
void key_and_value_args(benchmark::Benchmark* b) {
    b->ArgNames({"keys", "value_bytes"});
    for (const long keys : {1'000L, 1'000'000L}) {
        b->Args({keys, 16});
    }
    b->Args({1'000, 1'024});
    b->Args({1'000, 65'536});
}

// Lookup only: hashing, bucket walk, key comparison. No copy.
void BM_StoreContains(benchmark::State& state) {
    const FilledStore fixture{static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1))};
    std::size_t i = 0;
    for (auto _ : state) {
        bool found = fixture.store.contains(fixture.keys[fixture.order[i]]);
        benchmark::DoNotOptimize(found);
        if (++i == fixture.keys.size()) {
            i = 0;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StoreContains)->Apply(key_and_value_args);

// Lookup + copy of the value into a new std::string (what GET does).
void BM_StoreGetHit(benchmark::State& state) {
    const FilledStore fixture{static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1))};
    std::size_t i = 0;
    for (auto _ : state) {
        std::optional<std::string> value = fixture.store.get(fixture.keys[fixture.order[i]]);
        benchmark::DoNotOptimize(value);
        if (++i == fixture.keys.size()) {
            i = 0;
        }
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * state.range(1));
}
BENCHMARK(BM_StoreGetHit)->Apply(key_and_value_args);

void BM_StoreGetMiss(benchmark::State& state) {
    const FilledStore fixture{static_cast<std::size_t>(state.range(0)), 16};
    const std::vector<std::string> absent = make_keys(1'000);
    std::vector<std::string> missing;
    missing.reserve(absent.size());
    for (const std::string& key : absent) {
        missing.push_back("x" + key);   // same length class, never stored
    }
    std::size_t i = 0;
    for (auto _ : state) {
        std::optional<std::string> value = fixture.store.get(missing[i]);
        benchmark::DoNotOptimize(value);
        if (++i == missing.size()) {
            i = 0;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StoreGetMiss)->ArgName("keys")->Arg(1'000)->Arg(1'000'000);

// SET of an existing key. set() takes its arguments by value; the server moves
// the strings the parser already allocated into it. Here they are created in
// the loop, so the cost includes allocating them, as in the server.
void BM_StoreSetOverwrite(benchmark::State& state) {
    FilledStore fixture{static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1))};
    const std::string value(static_cast<std::size_t>(state.range(1)), 'w');
    std::size_t i = 0;
    for (auto _ : state) {
        fixture.store.set(fixture.keys[fixture.order[i]], value);
        if (++i == fixture.keys.size()) {
            i = 0;
        }
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_StoreSetOverwrite)->Apply(key_and_value_args);

// Building a store of N new keys from empty, including every rehash as the
// table grows, and destroying it. Reported per inserted key.
void BM_StoreInsertFresh(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    const std::vector<std::string> keys = make_keys(count);
    const std::string value(16, 'v');
    for (auto _ : state) {
        KvStore store;
        for (const std::string& key : keys) {
            store.set(key, value);
        }
        std::size_t size = store.size();
        benchmark::DoNotOptimize(size);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_StoreInsertFresh)->ArgName("keys")->Arg(1'000)->Arg(100'000)->Unit(benchmark::kMillisecond);

} // namespace
