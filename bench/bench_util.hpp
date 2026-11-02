#pragma once

// Helpers shared by the micro-benchmarks.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace cppkv::bench {

// "key:0000001234": fixed width, so every key has the same length and cost.
inline std::string key_for(std::size_t i) {
    std::string digits = std::to_string(i);
    return "key:" + std::string(10 - std::min<std::size_t>(digits.size(), 10), '0') + digits;
}

inline std::vector<std::string> make_keys(std::size_t count) {
    std::vector<std::string> keys;
    keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys.push_back(key_for(i));
    }
    return keys;
}

// Visiting keys in a fixed pseudo-random order, so that consecutive lookups
// do not hit neighbouring memory by accident. The seed is fixed: every run
// performs the same work.
inline std::vector<std::size_t> shuffled_indices(std::size_t count) {
    std::vector<std::size_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        order[i] = i;
    }
    std::mt19937_64 rng{42};
    std::shuffle(order.begin(), order.end(), rng);
    return order;
}

} // namespace cppkv::bench
