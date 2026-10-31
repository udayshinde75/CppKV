// Recovery and failure behaviour of the append-only log, on real files.

#include "cppkv/persistence/append_log.hpp"

#include "../support/temp_dir.hpp"
#include "cppkv/persistence/log_format.hpp"
#include "cppkv/server/command_dispatcher.hpp"

#include <gtest/gtest.h>

#include <csignal>
#include <sys/resource.h>

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fmt = cppkv::log_format;
using cppkv::test::read_file;
using cppkv::test::TempDir;
using cppkv::test::write_file;
using namespace std::string_literals;

namespace {

class AppendLogTest : public ::testing::Test {
protected:
    std::unique_ptr<cppkv::AppendLog> open(cppkv::KvStore& store, cppkv::RecoveryMode mode = cppkv::RecoveryMode::strict,
                                           cppkv::SyncPolicy sync = cppkv::SyncPolicy::always) {
        cppkv::LogOptions options;
        options.sync = sync;
        options.recovery = mode;
        return cppkv::AppendLog::open(path_, store, options, logger_, report_);
    }

    // Writes a log with these five records and returns the byte offset at
    // which each record ends (boundaries[0] = end of header).
    std::vector<std::uint64_t> write_reference_log() {
        std::vector<std::string> records{fmt::encode_set("a", "1"), fmt::encode_set("b", "two words"),
                                         fmt::encode_set("a", "3"), fmt::encode_del("b"),
                                         fmt::encode_set("empty", "")};
        std::string file = fmt::encode_header();
        std::vector<std::uint64_t> boundaries{file.size()};
        for (const auto& record : records) {
            file += record;
            boundaries.push_back(file.size());
        }
        write_file(path_, file);
        return boundaries;
    }

    // Checks `actual` against the state after the first `n` records of the
    // reference log. (KvStore is neither copyable nor movable by design, so the
    // expected state is built in place rather than returned.)
    static void expect_state_after(const cppkv::KvStore& actual, std::size_t n) {
        cppkv::KvStore expected;
        if (n >= 1) expected.set("a", "1");
        if (n >= 2) expected.set("b", "two words");
        if (n >= 3) expected.set("a", "3");
        if (n >= 4) expected.erase("b");
        if (n >= 5) expected.set("empty", "");
        expect_same(actual, expected);
    }

    static void expect_same(const cppkv::KvStore& actual, const cppkv::KvStore& expected) {
        EXPECT_EQ(actual.size(), expected.size());
        for (const char* key : {"a", "b", "empty"}) {
            EXPECT_EQ(actual.get(key), expected.get(key)) << "key " << key;
        }
    }

    TempDir dir_;
    std::filesystem::path path_ = dir_.path() / "cppkv.log";
    std::ostringstream log_output_;
    cppkv::Logger logger_{log_output_, cppkv::LogLevel::debug};
    cppkv::RecoveryReport report_;
};

// --- Normal operation --------------------------------------------------------

TEST_F(AppendLogTest, CreatesNewLogWithHeader) {
    cppkv::KvStore store;
    auto log = open(store);
    EXPECT_TRUE(report_.created);
    EXPECT_EQ(read_file(path_), fmt::encode_header());
    EXPECT_EQ(store.size(), 0U);
}

TEST_F(AppendLogTest, ReplaysSetsDeletesAndOverwritesInOrder) {
    {
        cppkv::KvStore store;
        auto log = open(store);
        ASSERT_TRUE(log->append_set("a", "1"));
        ASSERT_TRUE(log->append_set("b", "two words"));
        ASSERT_TRUE(log->append_set("a", "3"));   // duplicate update
        ASSERT_TRUE(log->append_del("b"));        // deletion
        ASSERT_TRUE(log->append_set("empty", ""));
    }
    cppkv::KvStore recovered;
    auto log = open(recovered);
    EXPECT_FALSE(report_.created);
    EXPECT_EQ(report_.records_applied, 5U);
    EXPECT_EQ(report_.discarded_bytes, 0U);
    expect_state_after(recovered, 5);
}

TEST_F(AppendLogTest, AppendingContinuesAfterRecovery) {
    {
        cppkv::KvStore store;
        auto log = open(store);
        ASSERT_TRUE(log->append_set("first", "1"));
    }
    {
        cppkv::KvStore store;
        auto log = open(store);
        ASSERT_TRUE(log->append_set("second", "2"));
    }
    cppkv::KvStore store;
    auto log = open(store);
    EXPECT_EQ(report_.records_applied, 2U);
    EXPECT_EQ(store.get("first"), "1");
    EXPECT_EQ(store.get("second"), "2");
}

TEST_F(AppendLogTest, BinaryKeysAndValuesSurvive) {
    {
        cppkv::KvStore store;
        auto log = open(store);
        ASSERT_TRUE(log->append_set("k\0ey"s, "\r\n\0\xff"s));
    }
    cppkv::KvStore store;
    auto log = open(store);
    EXPECT_EQ(store.get("k\0ey"s), "\r\n\0\xff"s);
}

TEST_F(AppendLogTest, IntervalAndNeverPoliciesPersistOnCleanShutdown) {
    for (const auto policy : {cppkv::SyncPolicy::interval, cppkv::SyncPolicy::never}) {
        std::filesystem::remove(path_);
        {
            cppkv::KvStore store;
            auto log = open(store, cppkv::RecoveryMode::strict, policy);
            ASSERT_TRUE(log->append_set("k", "v"));
        }   // the interval policy's destructor stops its thread and syncs
        cppkv::KvStore store;
        auto log = open(store);
        EXPECT_EQ(store.get("k"), "v") << cppkv::to_string(policy);
    }
}

TEST_F(AppendLogTest, SyncPolicyAlwaysSyncsEveryAppendAndNeverSyncsNone) {
    {
        cppkv::KvStore store;
        auto log = open(store, cppkv::RecoveryMode::strict, cppkv::SyncPolicy::always);
        for (int i = 0; i < 5; ++i) {
            ASSERT_TRUE(log->append_set("k", std::to_string(i)));
            EXPECT_EQ(log->syncs_performed(), static_cast<std::uint64_t>(i + 1)) << "sync before acknowledging";
        }
    }
    std::filesystem::remove(path_);
    cppkv::KvStore store;
    auto log = open(store, cppkv::RecoveryMode::strict, cppkv::SyncPolicy::never);
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(log->append_set("k", std::to_string(i)));
    }
    EXPECT_EQ(log->syncs_performed(), 0U);
}

TEST_F(AppendLogTest, SecondOpenIsRefusedWhileTheFirstHoldsTheLock) {
    cppkv::KvStore store;
    auto first = open(store);
    cppkv::KvStore other;
    EXPECT_THROW((void)open(other), std::system_error);
    first.reset();   // releases the lock
    EXPECT_NO_THROW((void)open(other));
}

// --- Crash-like damage at the end of the file --------------------------------

TEST_F(AppendLogTest, EveryCutPointRecoversTheCompletePrefix) {
    // Simulates a crash at every possible byte while writing the file: the
    // file is cut at each length from 0 to its full size. Recovery must keep
    // exactly the records that are complete, remove the rest, and leave a
    // file to which appending works.
    const auto boundaries = write_reference_log();
    const std::string full = read_file(path_);

    for (std::size_t cut = 0; cut <= full.size(); ++cut) {
        SCOPED_TRACE("cut at " + std::to_string(cut));
        write_file(path_, full.substr(0, cut));

        std::size_t complete = 0;
        while (complete + 1 < boundaries.size() && boundaries[complete + 1] <= cut) {
            ++complete;
        }
        const std::uint64_t expected_size = cut < fmt::kHeaderBytes ? fmt::kHeaderBytes : boundaries[complete];

        cppkv::KvStore store;
        auto log = open(store);
        ASSERT_EQ(report_.records_applied, complete);
        expect_state_after(store, complete);
        ASSERT_EQ(std::filesystem::file_size(path_), expected_size);
        EXPECT_EQ(report_.created, cut < fmt::kHeaderBytes);

        // The repaired file must accept new records.
        ASSERT_TRUE(log->append_set("after", "cut"));
        log.reset();
        cppkv::KvStore again;
        auto reopened = open(again);
        EXPECT_EQ(report_.records_applied, complete + 1);
        EXPECT_EQ(again.get("after"), "cut");
    }
}

TEST_F(AppendLogTest, ZeroFilledTailIsDiscarded) {
    const auto boundaries = write_reference_log();
    write_file(path_, read_file(path_) + std::string(4096, '\0'));

    cppkv::KvStore store;
    auto log = open(store);
    EXPECT_EQ(report_.records_applied, 5U);
    EXPECT_EQ(report_.discarded_bytes, 4096U);
    EXPECT_NE(report_.discard_reason.find("zero-filled tail"), std::string::npos) << report_.discard_reason;
    EXPECT_EQ(std::filesystem::file_size(path_), boundaries.back());
}

TEST_F(AppendLogTest, DamagedLastRecordIsTreatedAsTornWrite) {
    const auto boundaries = write_reference_log();
    std::string file = read_file(path_);
    file[file.size() - 1] ^= 0x40;   // flip one bit in the last record's payload
    write_file(path_, file);

    cppkv::KvStore store;
    auto log = open(store);
    EXPECT_EQ(report_.records_applied, 4U);
    EXPECT_NE(report_.discard_reason.find("checksum mismatch in last record"), std::string::npos)
        << report_.discard_reason;
    EXPECT_EQ(std::filesystem::file_size(path_), boundaries[4]);
}

// --- Damage in the middle: possible loss of acknowledged data ----------------

TEST_F(AppendLogTest, CorruptionInTheMiddleRefusesToStartAndLeavesFileUntouched) {
    const auto boundaries = write_reference_log();
    std::string file = read_file(path_);
    file[boundaries[1] + fmt::kRecordHeaderBytes + 2] ^= 0x01;   // inside record 2's payload
    write_file(path_, file);

    cppkv::KvStore store;
    try {
        (void)open(store);
        FAIL() << "corruption was not detected";
    } catch (const cppkv::LogCorruptedError& e) {
        EXPECT_EQ(e.offset(), boundaries[1]);
        EXPECT_NE(std::string{e.what()}.find("checksum mismatch"), std::string::npos) << e.what();
        EXPECT_NE(std::string{e.what()}.find("--recovery truncate"), std::string::npos) << e.what();
    }
    EXPECT_EQ(read_file(path_), file) << "strict mode must not modify the file";
}

TEST_F(AppendLogTest, TruncateModeKeepsTheValidPrefix) {
    const auto boundaries = write_reference_log();
    std::string file = read_file(path_);
    file[boundaries[1] + fmt::kRecordHeaderBytes + 2] ^= 0x01;
    write_file(path_, file);

    cppkv::KvStore store;
    auto log = open(store, cppkv::RecoveryMode::truncate);
    EXPECT_EQ(report_.records_applied, 1U);
    expect_state_after(store, 1);
    EXPECT_EQ(report_.discarded_bytes, file.size() - boundaries[1]);
    EXPECT_EQ(std::filesystem::file_size(path_), boundaries[1]);
}

TEST_F(AppendLogTest, AbsurdLengthInTheMiddleIsCorruption) {
    const auto boundaries = write_reference_log();
    std::string file = read_file(path_);
    const auto at = static_cast<std::size_t>(boundaries[1]);
    file.replace(at, 4, "\xff\xff\xff\x7f"s);   // record 2 claims ~2 GiB
    write_file(path_, file);

    cppkv::KvStore store;
    EXPECT_THROW((void)open(store), cppkv::LogCorruptedError);
}

TEST_F(AppendLogTest, ForeignFilesAreRejectedAndUntouched) {
    for (const std::string& foreign : {"hello world, not a log"s, "abc"s, "CPPKVLOX\x01\x00"s}) {
        write_file(path_, foreign);
        cppkv::KvStore store;
        EXPECT_THROW((void)open(store), cppkv::LogCorruptedError) << foreign;
        EXPECT_EQ(read_file(path_), foreign);
    }
}

TEST_F(AppendLogTest, UnsupportedVersionIsRejected) {
    write_file(path_, "CPPKVLOG\x02\x00"s);
    cppkv::KvStore store;
    EXPECT_THROW((void)open(store), cppkv::LogCorruptedError);
}

TEST_F(AppendLogTest, PartialHeaderOfANewLogIsCompleted) {
    write_file(path_, "CPPKV");   // crash while creating the log
    cppkv::KvStore store;
    auto log = open(store);
    EXPECT_TRUE(report_.created);
    EXPECT_EQ(read_file(path_), fmt::encode_header());
}

// --- Write failures: fail-stop -----------------------------------------------

void ignore_signal(int) {}

// Sets RLIMIT_FSIZE so that writes beyond `limit` bytes fail with EFBIG, a
// real I/O error injected without any test hook in the production code.
class FileSizeLimit {
public:
    explicit FileSizeLimit(rlim_t limit) {
        ::getrlimit(RLIMIT_FSIZE, &saved_);
        // Exceeding the limit raises SIGXFSZ, which by default kills the
        // process. With a handler installed, write() fails with EFBIG instead.
        previous_handler_ = std::signal(SIGXFSZ, ignore_signal);
        rlimit lowered = saved_;
        lowered.rlim_cur = limit;
        ::setrlimit(RLIMIT_FSIZE, &lowered);
    }
    ~FileSizeLimit() {
        ::setrlimit(RLIMIT_FSIZE, &saved_);
        std::signal(SIGXFSZ, previous_handler_);
    }
    FileSizeLimit(const FileSizeLimit&) = delete;
    FileSizeLimit& operator=(const FileSizeLimit&) = delete;

private:
    rlimit saved_{};
    void (*previous_handler_)(int) = nullptr;
};

TEST_F(AppendLogTest, WriteFailureRollsBackPartialRecordAndStopsWrites) {
    cppkv::KvStore store;
    auto log = open(store);
    cppkv::CommandDispatcher dispatcher{store, log.get()};
    ASSERT_EQ(dispatcher.execute({cppkv::CommandType::set, "kept", "1"}), cppkv::Response::ok());
    const std::uintmax_t size_before = std::filesystem::file_size(path_);

    {
        // Room for 10 more bytes: the next record (well over 10 bytes) is
        // written partially, then the write fails.
        const FileSizeLimit limit{size_before + 10};

        const auto response = dispatcher.execute({cppkv::CommandType::set, "lost", std::string(100, 'x')});
        EXPECT_EQ(response.kind, cppkv::Response::Kind::error);
        EXPECT_EQ(response.error, cppkv::ErrorCode::internal);
        EXPECT_NE(response.payload.find("write not persisted"), std::string::npos) << response.payload;
    }

    EXPECT_FALSE(log->healthy());
    EXPECT_NE(log->failure_reason().find("write failed"), std::string::npos) << log->failure_reason();
    EXPECT_EQ(std::filesystem::file_size(path_), size_before) << "the partial record must be rolled back";
    EXPECT_FALSE(store.contains("lost")) << "memory must not change when the log write failed";

    // Fail-stop: even a small write is refused now; reads still work.
    EXPECT_EQ(dispatcher.execute({cppkv::CommandType::set, "small", "1"}).kind, cppkv::Response::Kind::error);
    EXPECT_EQ(dispatcher.execute({cppkv::CommandType::del, "kept", ""}).kind, cppkv::Response::Kind::error);
    EXPECT_EQ(dispatcher.execute({cppkv::CommandType::get, "kept", ""}), cppkv::Response::value("1"));
    EXPECT_NE(log_output_.str().find("refusing further writes"), std::string::npos);

    log.reset();
    cppkv::KvStore recovered;
    auto reopened = open(recovered);
    EXPECT_EQ(report_.records_applied, 1U);
    EXPECT_EQ(report_.discarded_bytes, 0U);
    EXPECT_EQ(recovered.get("kept"), "1");
}

TEST_F(AppendLogTest, DeleteOfMissingKeyIsNotLogged) {
    cppkv::KvStore store;
    auto log = open(store);
    cppkv::CommandDispatcher dispatcher{store, log.get()};
    const auto size_before = log->size_bytes();
    EXPECT_EQ(dispatcher.execute({cppkv::CommandType::del, "nothing", ""}), cppkv::Response::integer_value(0));
    EXPECT_EQ(log->size_bytes(), size_before);
}

} // namespace
