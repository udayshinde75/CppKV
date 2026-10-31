#include "cppkv/server/session_registry.hpp"

#include "cppkv/common/logger.hpp"
#include "cppkv/server/client_session.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/store/kv_store.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

using AddResult = cppkv::SessionRegistry::AddResult;

namespace {

// Sessions here are never started: they only need to exist so the registry
// has something to point to. Their sockets are unopened.
class SessionRegistryTest : public ::testing::Test {
protected:
    std::shared_ptr<cppkv::ClientSession> make_session(cppkv::SessionRegistry& registry) {
        return std::make_shared<cppkv::ClientSession>(boost::asio::ip::tcp::socket{io_}, dispatcher_, logger_,
                                                      registry, cppkv::ClientSession::Options{});
    }

    boost::asio::io_context io_;
    std::ostringstream log_;
    cppkv::Logger logger_{log_};
    cppkv::KvStore store_;
    cppkv::CommandDispatcher dispatcher_{store_};
};

TEST_F(SessionRegistryTest, EnforcesMaximum) {
    cppkv::SessionRegistry registry{2};
    auto a = make_session(registry);
    auto b = make_session(registry);
    auto c = make_session(registry);

    EXPECT_EQ(registry.try_add(a), AddResult::added);
    EXPECT_EQ(registry.try_add(b), AddResult::added);
    EXPECT_EQ(registry.try_add(c), AddResult::full);
    EXPECT_EQ(registry.size(), 2U);
}

TEST_F(SessionRegistryTest, RemoveFreesSlotAndIsIdempotent) {
    cppkv::SessionRegistry registry{1};
    auto a = make_session(registry);
    auto b = make_session(registry);

    ASSERT_EQ(registry.try_add(a), AddResult::added);
    registry.remove(a.get());
    registry.remove(a.get());   // second remove is harmless
    registry.remove(b.get());   // never added: harmless
    EXPECT_EQ(registry.size(), 0U);
    EXPECT_EQ(registry.try_add(b), AddResult::added);
}

TEST_F(SessionRegistryTest, CloseCollectsLiveSessionsAndRefusesNewOnes) {
    cppkv::SessionRegistry registry{10};
    auto a = make_session(registry);
    auto b = make_session(registry);
    ASSERT_EQ(registry.try_add(a), AddResult::added);
    ASSERT_EQ(registry.try_add(b), AddResult::added);

    b.reset();   // b is destroyed without removing itself: only a weak_ptr remained

    const auto alive = registry.close_and_collect();
    ASSERT_EQ(alive.size(), 1U);
    EXPECT_EQ(alive.front(), a);
    EXPECT_TRUE(registry.is_closed());
    EXPECT_EQ(registry.try_add(make_session(registry)), AddResult::closed);
}

TEST_F(SessionRegistryTest, RegistryDoesNotKeepSessionsAlive) {
    cppkv::SessionRegistry registry{10};
    auto session = make_session(registry);
    const std::weak_ptr<cppkv::ClientSession> observer = session;
    ASSERT_EQ(registry.try_add(session), AddResult::added);

    session.reset();
    EXPECT_TRUE(observer.expired()) << "the registry must hold only weak references";
}

TEST_F(SessionRegistryTest, ConcurrentAddsNeverExceedTheLimit) {
    // 8 threads race to register 50 sessions each into a registry of 100.
    // Without check-and-insert under one lock, more than 100 could get in.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50;
    constexpr std::size_t kLimit = 100;
    cppkv::SessionRegistry registry{kLimit};

    std::vector<std::shared_ptr<cppkv::ClientSession>> sessions;
    for (int i = 0; i < kThreads * kPerThread; ++i) {
        sessions.push_back(make_session(registry));
    }

    std::atomic<std::size_t> added{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i) {
                    if (registry.try_add(sessions[static_cast<std::size_t>(t * kPerThread + i)]) == AddResult::added) {
                        ++added;
                    }
                }
            });
        }
    }
    EXPECT_EQ(added.load(), kLimit);
    EXPECT_EQ(registry.size(), kLimit);
}

} // namespace
