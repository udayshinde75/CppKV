#include "cppkv/store/kv_store.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

using namespace std::string_literals;       // "a\0b"s keeps the embedded NUL
using namespace std::string_view_literals;

namespace {

// --- Compile-time properties -------------------------------------------------

static_assert(!std::is_copy_constructible_v<cppkv::KvStore>, "a store must not be copied by accident");
static_assert(!std::is_copy_assignable_v<cppkv::KvStore>);
static_assert(!std::is_move_constructible_v<cppkv::KvStore>);
static_assert(std::is_same_v<decltype(std::declval<const cppkv::KvStore&>().get("k")),
                             std::optional<std::string>>,
              "get() returns an owning copy, not a reference or view");

// --- Empty store -------------------------------------------------------------

TEST(KvStore, NewStoreIsEmpty) {
    const cppkv::KvStore store;
    EXPECT_EQ(store.size(), 0U);
    EXPECT_FALSE(store.contains("a"));
    EXPECT_EQ(store.get("a"), std::nullopt);
}

TEST(KvStore, EraseOnEmptyStoreReturnsFalse) {
    cppkv::KvStore store;
    EXPECT_FALSE(store.erase("a"));
    EXPECT_EQ(store.size(), 0U);
}

// --- set / get ---------------------------------------------------------------

TEST(KvStore, SetNewKeyReturnsTrueAndValueIsReadable) {
    cppkv::KvStore store;
    EXPECT_TRUE(store.set("name", "hello"));
    EXPECT_EQ(store.get("name"), "hello");
    EXPECT_TRUE(store.contains("name"));
    EXPECT_EQ(store.size(), 1U);
}

TEST(KvStore, SetExistingKeyReplacesValueAndKeepsSize) {
    cppkv::KvStore store;
    ASSERT_TRUE(store.set("name", "hello"));
    EXPECT_FALSE(store.set("name", "world"));   // false: replaced, not inserted
    EXPECT_EQ(store.get("name"), "world");
    EXPECT_EQ(store.size(), 1U);
}

TEST(KvStore, ReplacingWithShorterValueDropsOldBytes) {
    cppkv::KvStore store;
    store.set("k", "a long original value");
    store.set("k", "x");
    EXPECT_EQ(store.get("k"), "x");
}

TEST(KvStore, EmptyValueIsDistinctFromMissingKey) {
    cppkv::KvStore store;
    store.set("empty", "");

    const std::optional<std::string> present = store.get("empty");
    ASSERT_TRUE(present.has_value()) << "an empty value must not look like a missing key";
    EXPECT_EQ(*present, "");
    EXPECT_TRUE(store.contains("empty"));

    EXPECT_FALSE(store.get("missing").has_value());
}

TEST(KvStore, ValuesMayContainSpaces) {
    cppkv::KvStore store;
    store.set("city", "Frankfurt am Main");
    store.set("gap", "two  spaces ");
    EXPECT_EQ(store.get("city"), "Frankfurt am Main");
    EXPECT_EQ(store.get("gap"), "two  spaces ");
}

TEST(KvStore, KeysAreCaseSensitive) {
    cppkv::KvStore store;
    store.set("Key", "upper");
    store.set("key", "lower");
    EXPECT_EQ(store.size(), 2U);
    EXPECT_EQ(store.get("Key"), "upper");
    EXPECT_EQ(store.get("key"), "lower");
    EXPECT_FALSE(store.contains("KEY"));
}

TEST(KvStore, StoresArbitraryBytesExactly) {
    cppkv::KvStore store;
    const std::string value = "a\0b\xff\r\n"s;   // 6 bytes, including NUL
    ASSERT_EQ(value.size(), 6U);

    store.set("bin", value);
    const auto stored = store.get("bin");
    ASSERT_TRUE(stored.has_value());
    EXPECT_EQ(stored->size(), 6U);
    EXPECT_EQ(*stored, value);
}

TEST(KvStore, KeysDifferingOnlyAfterNulAreDistinct) {
    // A C-string based implementation (strlen, strcmp) would see both keys as "k".
    cppkv::KvStore store;
    store.set("k\0a"s, "first");
    store.set("k\0b"s, "second");
    EXPECT_EQ(store.size(), 2U);
    EXPECT_EQ(store.get("k\0a"sv), "first");
    EXPECT_EQ(store.get("k\0b"sv), "second");
    EXPECT_FALSE(store.contains("k"));
}

TEST(KvStore, EmptyKeyIsAcceptedBecauseValidationBelongsToTheParser) {
    // Documents the layering decision: the protocol forbids empty keys, the
    // store does not. See kv_store.hpp.
    cppkv::KvStore store;
    EXPECT_TRUE(store.set("", "v"));
    EXPECT_EQ(store.get(""), "v");
}

TEST(KvStore, LargeValueRoundTrips) {
    cppkv::KvStore store;
    std::string large(1024 * 1024, '\0');   // 1 MiB
    for (std::size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<char>('a' + i % 26);
    }
    store.set("big", large);
    EXPECT_EQ(store.get("big"), large);
}

// --- Ownership ---------------------------------------------------------------

TEST(KvStore, GetReturnsIndependentCopy) {
    cppkv::KvStore store;
    store.set("k", "original");

    std::optional<std::string> copy = store.get("k");
    ASSERT_TRUE(copy.has_value());
    *copy = "modified by caller";

    EXPECT_EQ(store.get("k"), "original");
}

TEST(KvStore, SetCopiesLvalueArgumentsAndLeavesThemIntact) {
    cppkv::KvStore store;
    std::string key = "k";
    std::string value = "v";

    store.set(key, value);   // lvalues: copied into the by-value parameters

    EXPECT_EQ(key, "k");
    EXPECT_EQ(value, "v");
    value = "changed";       // the store holds its own copy
    EXPECT_EQ(store.get("k"), "v");
}

TEST(KvStore, LookupWorksWithViewIntoLargerBuffer) {
    // How the Phase 4 parser will call the store: with a view into the receive
    // buffer, without allocating a std::string for the key.
    cppkv::KvStore store;
    store.set("user:42", "Uday");

    const std::string receive_buffer = "GET user:42\r\nGET other\r\n";
    const std::string_view key = std::string_view{receive_buffer}.substr(4, 7);
    ASSERT_EQ(key, "user:42");

    EXPECT_EQ(store.get(key), "Uday");
    EXPECT_TRUE(store.contains(key));
}

// --- erase -------------------------------------------------------------------

TEST(KvStore, EraseRemovesExistingKey) {
    cppkv::KvStore store;
    store.set("k", "v");
    EXPECT_TRUE(store.erase("k"));
    EXPECT_FALSE(store.contains("k"));
    EXPECT_EQ(store.get("k"), std::nullopt);
    EXPECT_EQ(store.size(), 0U);
}

TEST(KvStore, EraseTwiceReturnsTrueThenFalse) {
    cppkv::KvStore store;
    store.set("k", "v");
    EXPECT_TRUE(store.erase("k"));
    EXPECT_FALSE(store.erase("k"));
}

TEST(KvStore, EraseMissingKeyLeavesOtherKeysIntact) {
    cppkv::KvStore store;
    store.set("a", "1");
    store.set("b", "2");
    EXPECT_FALSE(store.erase("c"));
    EXPECT_EQ(store.size(), 2U);
    EXPECT_EQ(store.get("a"), "1");
    EXPECT_EQ(store.get("b"), "2");
}

TEST(KvStore, KeyCanBeReinsertedAfterErase) {
    cppkv::KvStore store;
    store.set("k", "old");
    store.erase("k");
    EXPECT_TRUE(store.set("k", "new"));   // true: inserted again, not replaced
    EXPECT_EQ(store.get("k"), "new");
}

// --- size and many keys ------------------------------------------------------

TEST(KvStore, SizeTracksMixedOperations) {
    cppkv::KvStore store;
    store.set("a", "1");   // 1
    store.set("b", "2");   // 2
    store.set("a", "3");   // 2 (replace)
    store.erase("b");      // 1
    store.erase("zzz");    // 1 (missing)
    store.set("c", "");    // 2 (empty value still counts)
    EXPECT_EQ(store.size(), 2U);
}

TEST(KvStore, ManyKeysSurviveRehashing) {
    // 10'000 inserts force the hash table to rehash several times; every
    // key must still map to its own value afterwards.
    constexpr int kCount = 10'000;
    cppkv::KvStore store;
    for (int i = 0; i < kCount; ++i) {
        ASSERT_TRUE(store.set("key:" + std::to_string(i), "value:" + std::to_string(i)));
    }
    ASSERT_EQ(store.size(), static_cast<std::size_t>(kCount));

    for (int i = 0; i < kCount; i += 2) {
        ASSERT_TRUE(store.erase("key:" + std::to_string(i)));
    }
    EXPECT_EQ(store.size(), static_cast<std::size_t>(kCount / 2));

    for (int i = 0; i < kCount; ++i) {
        const auto value = store.get("key:" + std::to_string(i));
        if (i % 2 == 0) {
            ASSERT_FALSE(value.has_value()) << "key:" << i;
        } else {
            ASSERT_EQ(value, "value:" + std::to_string(i)) << "key:" << i;
        }
    }
}

} // namespace
