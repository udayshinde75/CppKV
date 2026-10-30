#include "cppkv/server/command_dispatcher.hpp"

#include <gtest/gtest.h>

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

} // namespace
