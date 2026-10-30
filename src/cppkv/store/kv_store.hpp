#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cppkv {

// In-memory map from byte-string keys to byte-string values.
//
// Semantics
// - Keys and values are arbitrary byte strings: they may be empty and may
//   contain any byte, including '\0'. The store does NOT validate them.
//   Protocol rules (allowed characters, length limits) belong to the request
//   parser, which rejects bad input before it reaches the store. Keeping the
//   store rule-free means the persistence layer can replay any record it reads.
// - An empty value is a value: get() on a key holding "" returns an engaged
//   optional containing "", which is distinct from a missing key (nullopt).
// - Keys are compared byte by byte, so they are case-sensitive.
//
// Ownership
// - The store owns copies of all keys and values. get() returns a copy, never
//   a reference or view into the map: from Phase 5 on, a reference could
//   dangle as soon as another thread modifies the map.
//
// Thread safety
// - None. A KvStore must not be used from several threads at once.
//   Synchronisation is added deliberately in Phase 5.
//
// Exception safety
// - Mutating operations give the strong guarantee: if set() throws (only
//   std::bad_alloc is possible), the store is unchanged. erase() never throws.
class KvStore {
public:
    KvStore() = default;

    // A store is an identity object (one database per server). Copying it by
    // accident, e.g. by passing it by value, would silently fork the data, so
    // copying is disabled. Declaring the copy operations also suppresses the
    // implicit move operations, so a KvStore is neither copyable nor movable.
    KvStore(const KvStore&) = delete;
    KvStore& operator=(const KvStore&) = delete;

    // Inserts `key` with `value`, or replaces the value if `key` exists.
    // Returns true if the key was newly inserted, false if it was replaced.
    //
    // Parameters are taken by value ("sink" parameters) and moved into the map:
    // a caller passing temporaries pays for zero copies, a caller passing
    // lvalues pays for exactly one copy each, and the caller's objects are
    // left untouched.
    bool set(std::string key, std::string value);

    // Returns a copy of the value for `key`, or std::nullopt if it is missing.
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;

    // Removes `key`. Returns true if it existed, false otherwise.
    bool erase(std::string_view key);

    [[nodiscard]] bool contains(std::string_view key) const;

    [[nodiscard]] std::size_t size() const noexcept;

private:
    // "Transparent" hash: lets find()/contains() accept a std::string_view
    // directly (C++20 heterogeneous lookup), so looking up a key that is a
    // view into a network buffer needs no temporary std::string allocation.
    //
    // Invariant: equal keys must hash equally whatever their type. Both
    // std::string and std::string_view keys are hashed through this single
    // string_view overload, which guarantees it.
    struct TransparentStringHash {
        using is_transparent = void;

        std::size_t operator()(std::string_view text) const noexcept {
            return std::hash<std::string_view>{}(text);
        }
    };

    // std::equal_to<> (the void specialisation) is the matching transparent
    // equality: it compares std::string with std::string_view without copying.
    std::unordered_map<std::string, std::string, TransparentStringHash, std::equal_to<>> entries_;
};

} // namespace cppkv
