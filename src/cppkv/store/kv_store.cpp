#include "cppkv/store/kv_store.hpp"

#include <utility>

namespace cppkv {

bool KvStore::set(std::string key, std::string value) {
    // insert_or_assign (C++17):
    //  - key missing: allocates a node holding the moved key and value. If the
    //    allocation throws, the standard guarantees the map is unchanged.
    //  - key present: move-assigns the value (noexcept for std::string); the
    //    key argument is not used.
    // Either way the store is left unchanged if anything throws: strong guarantee.
    return entries_.insert_or_assign(std::move(key), std::move(value)).second;
}

std::optional<std::string> KvStore::get(std::string_view key) const {
    const auto it = entries_.find(key);   // heterogeneous lookup, no allocation
    if (it == entries_.end()) {
        return std::nullopt;
    }
    return it->second;   // copy of the value; the caller owns it
}

bool KvStore::erase(std::string_view key) {
    // unordered_map::erase(const K&) with a heterogeneous key is C++23, so
    // find first (heterogeneous in C++20), then erase by iterator.
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return false;
    }
    entries_.erase(it);   // erase(iterator) never throws
    return true;
}

bool KvStore::contains(std::string_view key) const {
    return entries_.contains(key);
}

std::size_t KvStore::size() const noexcept {
    return entries_.size();
}

} // namespace cppkv
