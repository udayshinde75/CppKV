// End-to-end persistence: data written over TCP survives a server restart and
// a real crash (SIGKILL) of the server process.

#include "../support/temp_dir.hpp"
#include "server_process.hpp"
#include "test_client.hpp"

#include "cppkv/common/logger.hpp"
#include "cppkv/persistence/append_log.hpp"
#include "cppkv/persistence/log_format.hpp"
#include "cppkv/server/command_dispatcher.hpp"
#include "cppkv/server/tcp_server.hpp"
#include "cppkv/store/kv_store.hpp"

#include <gtest/gtest.h>

#include <sys/wait.h>

#include <memory>
#include <sstream>
#include <string>
#include <thread>

#ifndef CPPKV_SERVER_BINARY
#error "CPPKV_SERVER_BINARY must be defined by the build"
#endif

using cppkv::test::ServerProcess;
using cppkv::test::TempDir;
using cppkv::test::TestClient;

namespace {

// --- In-process restart ------------------------------------------------------

// One server "lifetime": open the log (recovering), serve, stop.
struct ServerInstance {
    explicit ServerInstance(const std::filesystem::path& log_path) {
        cppkv::LogOptions options;
        log = cppkv::AppendLog::open(log_path, store, options, logger, report);
        dispatcher = std::make_unique<cppkv::CommandDispatcher>(store, log.get());
        cppkv::ServerConfig config;
        config.port = 0;
        server = std::make_unique<cppkv::TcpServer>(config, *dispatcher, logger);
        thread = std::jthread{[this] { server->run(); }};
    }
    ~ServerInstance() {
        server->stop();
        thread.join();
    }
    ServerInstance(const ServerInstance&) = delete;
    ServerInstance& operator=(const ServerInstance&) = delete;

    std::ostringstream output;
    cppkv::Logger logger{output};
    cppkv::KvStore store;
    cppkv::RecoveryReport report;
    std::unique_ptr<cppkv::AppendLog> log;
    std::unique_ptr<cppkv::CommandDispatcher> dispatcher;
    std::unique_ptr<cppkv::TcpServer> server;
    std::jthread thread;
};

TEST(Persistence, DataSurvivesServerRestart) {
    TempDir dir;
    const auto log_path = dir.path() / "cppkv.log";
    {
        ServerInstance first{log_path};
        TestClient client{first.server->local_endpoint()};
        client.send("SET city Frankfurt am Main\nSET n 1\nSET n 2\nSET gone x\nDEL gone\nSET empty \n");
        for (int i = 0; i < 4; ++i) {
            ASSERT_EQ(client.read_line(), "OK");   // SET city, SET n, SET n, SET gone
        }
        ASSERT_EQ(client.read_line(), "INT 1");    // DEL gone
        ASSERT_EQ(client.read_line(), "OK");       // SET empty
    }

    ServerInstance second{log_path};
    EXPECT_EQ(second.report.records_applied, 6U);
    TestClient client{second.server->local_endpoint()};
    client.send("GET city\nGET n\nEXISTS gone\nGET empty\nDBSIZE\n");
    EXPECT_EQ(client.read_line(), "VALUE 17 Frankfurt am Main");
    EXPECT_EQ(client.read_line(), "VALUE 1 2");
    EXPECT_EQ(client.read_line(), "INT 0");
    EXPECT_EQ(client.read_line(), "VALUE 0 ");
    EXPECT_EQ(client.read_line(), "INT 3");
}

TEST(Persistence, ConcurrentWritersAreAllRecovered) {
    // 8 clients on 8 threads write 100 keys each; every OK must be in the log.
    TempDir dir;
    const auto log_path = dir.path() / "cppkv.log";
    {
        ServerInstance first{log_path};
        std::vector<std::jthread> writers;
        for (int c = 0; c < 8; ++c) {
            writers.emplace_back([&, c] {
                TestClient client{first.server->local_endpoint()};
                std::string burst;
                for (int k = 0; k < 100; ++k) {
                    burst += "SET c" + std::to_string(c) + "k" + std::to_string(k) + " " + std::to_string(k) + "\n";
                }
                client.send(burst);
                for (int k = 0; k < 100; ++k) {
                    EXPECT_EQ(client.read_line(), "OK");
                }
            });
        }
    }
    ServerInstance second{log_path};
    EXPECT_EQ(second.report.records_applied, 800U);
    EXPECT_EQ(second.store.size(), 800U);
    EXPECT_EQ(second.store.get("c7k99"), "99");
}

// --- Real process crashes ----------------------------------------------------

class CrashTest : public ::testing::Test {
protected:
    bool start(ServerProcess& process, std::vector<std::string> extra = {}) {
        std::vector<std::string> args{"--port", "0", "--data-dir", dir_.path().string(), "--threads", "2"};
        args.insert(args.end(), extra.begin(), extra.end());
        return process.start(CPPKV_SERVER_BINARY, args);
    }

    TempDir dir_;
};

TEST_F(CrashTest, AcknowledgedWritesSurviveSigkillWithSyncAlways) {
    {
        ServerProcess server;
        ASSERT_TRUE(start(server, {"--sync", "always"})) << server.stderr_text();
        TestClient client{server.endpoint()};
        for (int i = 0; i < 50; ++i) {
            client.send("SET key" + std::to_string(i) + " value" + std::to_string(i) + "\n");
            ASSERT_EQ(client.read_line(), "OK");   // acknowledged
        }
        const int status = server.kill_and_wait(SIGKILL);   // no destructors, no flushing: a crash
        ASSERT_TRUE(WIFSIGNALED(status));
        ASSERT_EQ(WTERMSIG(status), SIGKILL);
    }

    ServerProcess restarted;
    ASSERT_TRUE(start(restarted)) << restarted.stderr_text();
    EXPECT_NE(restarted.stderr_text().find("recovered 50 record(s)"), std::string::npos) << restarted.stderr_text();
    TestClient client{restarted.endpoint()};
    client.send("DBSIZE\nGET key0\nGET key49\n");
    EXPECT_EQ(client.read_line(), "INT 50");
    EXPECT_EQ(client.read_line(), "VALUE 6 value0");
    EXPECT_EQ(client.read_line(), "VALUE 7 value49");
}

TEST_F(CrashTest, SyncNeverAlsoSurvivesAProcessCrash) {
    // Without fsync, written data sits in the OS page cache. A process crash
    // does not lose it (the kernel still writes it out); only an OS crash or
    // power loss can. That difference is why --sync never is weaker than
    // always, yet passes this test.
    {
        ServerProcess server;
        ASSERT_TRUE(start(server, {"--sync", "never"})) << server.stderr_text();
        TestClient client{server.endpoint()};
        client.send("SET a 1\nSET b 2\nDEL a\n");
        ASSERT_EQ(client.read_line(), "OK");
        ASSERT_EQ(client.read_line(), "OK");
        ASSERT_EQ(client.read_line(), "INT 1");
        server.kill_and_wait(SIGKILL);
    }
    ServerProcess restarted;
    ASSERT_TRUE(start(restarted)) << restarted.stderr_text();
    TestClient client{restarted.endpoint()};
    client.send("EXISTS a\nGET b\n");
    EXPECT_EQ(client.read_line(), "INT 0");
    EXPECT_EQ(client.read_line(), "VALUE 1 2");
}

TEST_F(CrashTest, TornTailFromACrashIsRepairedOnStartup) {
    {
        ServerProcess server;
        ASSERT_TRUE(start(server)) << server.stderr_text();
        TestClient client{server.endpoint()};
        client.send("SET a 1\nSET b 2\n");
        ASSERT_EQ(client.read_line(), "OK");
        ASSERT_EQ(client.read_line(), "OK");
        server.kill_and_wait(SIGKILL);
    }
    // Emulate a crash in the middle of the next append: half a record.
    const auto log_path = dir_.path() / "cppkv.log";
    const std::string partial = cppkv::log_format::encode_set("c", "3").substr(0, 7);
    cppkv::test::write_file(log_path, cppkv::test::read_file(log_path) + partial);

    ServerProcess restarted;
    ASSERT_TRUE(start(restarted)) << restarted.stderr_text();
    EXPECT_NE(restarted.stderr_text().find("discarded 7 byte(s)"), std::string::npos) << restarted.stderr_text();
    TestClient client{restarted.endpoint()};
    client.send("DBSIZE\nGET c\n");
    EXPECT_EQ(client.read_line(), "INT 2");
    EXPECT_EQ(client.read_line(), "NIL");
}

TEST_F(CrashTest, CorruptedLogStopsStartupUnlessTruncateIsRequested) {
    {
        ServerProcess server;
        ASSERT_TRUE(start(server)) << server.stderr_text();
        TestClient client{server.endpoint()};
        client.send("SET a 1\nSET b 2\nSET c 3\n");
        for (int i = 0; i < 3; ++i) {
            ASSERT_EQ(client.read_line(), "OK");
        }
        server.kill_and_wait(SIGKILL);
    }
    const auto log_path = dir_.path() / "cppkv.log";
    std::string bytes = cppkv::test::read_file(log_path);
    bytes[cppkv::log_format::kHeaderBytes + 8 + 3] ^= 0x01;   // inside the first record
    cppkv::test::write_file(log_path, bytes);

    {
        ServerProcess refused;
        EXPECT_FALSE(start(refused)) << "server must refuse a corrupted log";
        const int status = refused.wait();
        EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 1) << refused.stderr_text();
        EXPECT_NE(refused.stderr_text().find("is corrupted"), std::string::npos) << refused.stderr_text();
        EXPECT_EQ(cppkv::test::read_file(log_path), bytes) << "the corrupted log must be left untouched";
    }

    ServerProcess repaired;
    ASSERT_TRUE(start(repaired, {"--recovery", "truncate"})) << repaired.stderr_text();
    TestClient client{repaired.endpoint()};
    client.send("DBSIZE\n");
    EXPECT_EQ(client.read_line(), "INT 0") << "everything from the damaged record on is discarded";
}

TEST_F(CrashTest, SecondServerOnTheSameDataDirectoryIsRefused) {
    ServerProcess first;
    ASSERT_TRUE(start(first)) << first.stderr_text();
    ServerProcess second;
    EXPECT_FALSE(start(second));
    second.wait();
    EXPECT_NE(second.stderr_text().find("in use by another process"), std::string::npos) << second.stderr_text();
}

} // namespace
