// The load generator against a real in-process server: it must count, time
// and validate what the server actually did, or its numbers mean nothing.

#include "loadgen/load_generator.hpp"
#include "loadgen/load_options.hpp"

#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/tcp_server.hpp"
#include "cppkv/store/kv_store.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

using cppkv::loadgen::LoadOptions;
using cppkv::loadgen::LoadReport;

namespace {

class LoadgenTest : public ::testing::Test {
protected:
    void start_server(std::size_t max_connections = 128) {
        cppkv::ServerConfig config;
        config.port = 0;
        config.io_threads = 2;
        config.idle_timeout_seconds = 0;
        config.max_connections = max_connections;
        server_ = std::make_unique<cppkv::TcpServer>(config, dispatcher_, logger_);
        server_thread_ = std::jthread{[this] { server_->run(); }};
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
    }

    [[nodiscard]] LoadOptions short_run() const {
        LoadOptions o;
        o.port = server_->local_endpoint().port();
        o.clients = 4;
        o.duration = std::chrono::milliseconds{500};
        o.warmup = std::chrono::milliseconds{100};
        o.keys = 1'000;
        o.value_bytes = 32;
        return o;
    }

    std::ostringstream log_;
    cppkv::Logger logger_{log_};
    cppkv::KvStore store_;
    cppkv::CommandDispatcher dispatcher_{store_};
    std::unique_ptr<cppkv::TcpServer> server_;
    std::jthread server_thread_;   // declared last: joined before the server is destroyed
};

TEST_F(LoadgenTest, ClosedLoopCountsMatchAndEveryReplyIsValid) {
    start_server();
    LoadOptions o = short_run();
    o.pipeline = 4;
    o.read_percent = 50;
    const LoadReport r = cppkv::loadgen::run_load(o);

    ASSERT_TRUE(r.failure.empty()) << r.failure;
    EXPECT_GT(r.requests, 0U);
    EXPECT_EQ(r.gets + r.sets, r.requests);
    EXPECT_GT(r.gets, 0U);
    EXPECT_GT(r.sets, 0U);
    EXPECT_EQ(r.errors, 0U);
    EXPECT_EQ(r.get_misses, 0U) << "every key was populated";
    EXPECT_EQ(r.latency.count(), r.requests) << "one latency sample per measured request";
    EXPECT_GT(r.latency.min(), 0U);
    // Populated keys only: SETs overwrite, never add.
    EXPECT_EQ(store_.size(), 1'000U);
    EXPECT_EQ(store_.get("key:0000000000"), std::string(32, 'v'));
    EXPECT_EQ(store_.get("key:0000000999"), std::string(32, 'v'));
    EXPECT_FALSE(store_.contains("key:0000001000"));
}

TEST_F(LoadgenTest, OpenLoopSendsExactlyTheScheduledNumberOfRequests) {
    start_server();
    LoadOptions o = short_run();
    o.clients = 2;
    o.rate = 2'000;   // per second, over both connections
    o.duration = std::chrono::seconds{1};
    const LoadReport r = cppkv::loadgen::run_load(o);

    ASSERT_TRUE(r.failure.empty()) << r.failure;
    // The schedule puts 2000 send times into the 1 s window (±1 per
    // connection at the window's edges). Any lost or extra reply shows here.
    EXPECT_NEAR(static_cast<double>(r.requests), 2'000.0, 2.0);
    EXPECT_EQ(r.latency.count(), r.requests);
    EXPECT_EQ(r.errors, 0U);
}

TEST_F(LoadgenTest, GetsOnAnEmptyStoreAreCountedAsMisses) {
    start_server();
    LoadOptions o = short_run();
    o.populate = false;
    o.read_percent = 100;
    const LoadReport r = cppkv::loadgen::run_load(o);

    ASSERT_TRUE(r.failure.empty()) << r.failure;
    EXPECT_GT(r.gets, 0U);
    EXPECT_EQ(r.get_misses, r.gets);
    EXPECT_EQ(r.sets, 0U);
    EXPECT_EQ(store_.size(), 0U);
}

TEST_F(LoadgenTest, ServerStoppingMidRunIsReportedAsAFailure) {
    start_server();
    LoadOptions o = short_run();
    o.duration = std::chrono::seconds{20};
    auto running = std::async(std::launch::async, [o] { return cppkv::loadgen::run_load(o); });
    std::this_thread::sleep_for(std::chrono::milliseconds{400});
    server_->stop();

    ASSERT_EQ(running.wait_for(std::chrono::seconds{10}), std::future_status::ready)
        << "the load generator must notice the lost connections, not wait for the end of the run";
    const LoadReport r = running.get();
    EXPECT_FALSE(r.failure.empty());
}

TEST_F(LoadgenTest, ConnectionLimitIsReportedAsAFailure) {
    start_server(/*max_connections=*/2);
    LoadOptions o = short_run();
    o.populate = false;
    o.clients = 4;
    const LoadReport r = cppkv::loadgen::run_load(o);
    EXPECT_FALSE(r.failure.empty());
}

TEST(LoadgenNoServer, ConnectionRefusedIsAFailureNotAnException) {
    // A port that was free a moment ago: nothing listens on it now.
    std::uint16_t port = 0;
    {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acceptor{io, {boost::asio::ip::make_address("127.0.0.1"), 0}};
        port = acceptor.local_endpoint().port();
    }
    LoadOptions o;
    o.port = port;
    const LoadReport r = cppkv::loadgen::run_load(o);
    EXPECT_FALSE(r.failure.empty());
}

} // namespace
