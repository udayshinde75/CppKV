#include "cppkv/server/command_dispatcher.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using cppkv::CommandType;
using cppkv::Request;
using cppkv::Response;

namespace {

class CommandDispatcherTest : public ::testing::Test {
protected:
    Response run(CommandType command, std::string key = {}, std::string value = {}) {
        return dispatcher_.execute(Request{command, std::move(key), std::move(value)});
    }

    cppkv::KvStore store_;
    cppkv::CommandDispatcher dispatcher_{store_};
};

TEST_F(CommandDispatcherTest, Ping) {
    EXPECT_EQ(run(CommandType::ping), Response::pong());
}

TEST_F(CommandDispatcherTest, SetThenGet) {
    EXPECT_EQ(run(CommandType::set, "k", "v"), Response::ok());
    EXPECT_EQ(run(CommandType::get, "k"), Response::value("v"));
    EXPECT_EQ(store_.get("k"), "v");
}

TEST_F(CommandDispatcherTest, SetReplacesAndStillAnswersOk) {
    run(CommandType::set, "k", "old");
    EXPECT_EQ(run(CommandType::set, "k", "new"), Response::ok());
    EXPECT_EQ(run(CommandType::get, "k"), Response::value("new"));
}

TEST_F(CommandDispatcherTest, GetMissingIsNilAndEmptyValueIsValue) {
    EXPECT_EQ(run(CommandType::get, "missing"), Response::nil());
    run(CommandType::set, "empty", "");
    EXPECT_EQ(run(CommandType::get, "empty"), Response::value(""));
}

TEST_F(CommandDispatcherTest, DelReportsWhetherKeyExisted) {
    run(CommandType::set, "k", "v");
    EXPECT_EQ(run(CommandType::del, "k"), Response::integer_value(1));
    EXPECT_EQ(run(CommandType::del, "k"), Response::integer_value(0));
}

TEST_F(CommandDispatcherTest, Exists) {
    EXPECT_EQ(run(CommandType::exists, "k"), Response::integer_value(0));
    run(CommandType::set, "k", "");
    EXPECT_EQ(run(CommandType::exists, "k"), Response::integer_value(1));
}

TEST_F(CommandDispatcherTest, DbSize) {
    EXPECT_EQ(run(CommandType::dbsize), Response::integer_value(0));
    run(CommandType::set, "a", "1");
    run(CommandType::set, "b", "2");
    run(CommandType::set, "a", "3");
    EXPECT_EQ(run(CommandType::dbsize), Response::integer_value(2));
}

TEST_F(CommandDispatcherTest, QuitAnswersByeAndDoesNotTouchStore) {
    run(CommandType::set, "k", "v");
    EXPECT_EQ(run(CommandType::quit), Response::bye());
    EXPECT_EQ(store_.size(), 1U);
}

TEST(CommandDispatcherConcurrency, ParallelCommandsAreAtomic) {
    // 8 threads hammer one dispatcher with SET/GET/DEL on their own
    // keys and SET/GET on shared keys. Each thread checks it reads its own
    // writes. Built with -fsanitize=thread, this test fails if execute() is
    // not synchronised; without TSan it may crash or lose keys instead.
    cppkv::KvStore store;
    cppkv::CommandDispatcher dispatcher{store};
    constexpr int kThreads = 8;
    constexpr int kOps = 2000;
    std::atomic<int> failures{0};
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&, t] {
                const std::string prefix = "t" + std::to_string(t) + ":";
                for (int i = 0; i < kOps; ++i) {
                    const std::string key = prefix + std::to_string(i);
                    const std::string value = std::to_string(i);
                    if (dispatcher.execute(Request{CommandType::set, key, value}) != Response::ok() ||
                        dispatcher.execute(Request{CommandType::get, key, ""}) != Response::value(value) ||
                        dispatcher.execute(Request{CommandType::set, "shared" + std::to_string(i % 10), prefix}) !=
                            Response::ok()) {
                        ++failures;
                    }
                    if (i % 2 == 0 &&
                        dispatcher.execute(Request{CommandType::del, key, ""}) != Response::integer_value(1)) {
                        ++failures;
                    }
                }
            });
        }
    }
    EXPECT_EQ(failures.load(), 0);
    // Each thread kept every odd key; plus 10 shared keys.
    EXPECT_EQ(store.size(), static_cast<std::size_t>(kThreads * kOps / 2 + 10));
}

} // namespace
