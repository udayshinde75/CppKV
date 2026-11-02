#pragma once

#include "cppkv/common/config.hpp"
#include "cppkv/common/logger.hpp"
#include "cppkv/persistence/posix_file.hpp"
#include "cppkv/store/kv_store.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace cppkv {

// Startup refused: the log is damaged in a way that automatic repair could
// lose acknowledged data, or the file is not a CppKV log at all. The file has
// not been modified.
class LogCorruptedError : public std::runtime_error {
public:
    // const&: std::runtime_error copies the message anyway (it has no
    // std::string&& constructor), so taking it by value and moving gained nothing.
    LogCorruptedError(const std::string& message, std::uint64_t offset)
        : std::runtime_error{message}, offset_{offset} {}
    [[nodiscard]] std::uint64_t offset() const noexcept { return offset_; }

private:
    std::uint64_t offset_;
};

struct RecoveryReport {
    std::uint64_t records_applied = 0;
    std::uint64_t valid_bytes = 0;       // file size after recovery
    std::uint64_t discarded_bytes = 0;   // removed from the end of the file
    std::string discard_reason;          // why, if discarded_bytes > 0
    bool created = false;                // a new, empty log was initialised
};

struct LogOptions {
    SyncPolicy sync = SyncPolicy::always;
    std::chrono::milliseconds sync_interval{1000};
    RecoveryMode recovery = RecoveryMode::strict;
};

// Append-only log of mutations (SET, DEL) that makes the store survive restarts.
//
// Write path (called by CommandDispatcher under its mutex):
//   encode record -> write() -> sync per policy -> return true
//   -> caller applies the change to memory -> caller sends OK.
// So a client never sees OK for a change that is not in the log.
//
// Recovery (open()): replays every valid record into the store. A damaged
// TAIL — what a crash in the middle of an append leaves behind — is removed
// automatically. Damage FOLLOWED BY further data means something other than a
// crash (bit rot, a bug, manual edits). Repairing that automatically would
// silently drop acknowledged writes, so strict mode refuses to start instead.
//
// Fail-stop on write errors: if a write or sync fails, the partial record is
// truncated away and the log refuses every further append (append() returns
// false). After a failed fsync the OS may have dropped the dirty pages and a
// retried fsync can report success for data that was never written — the
// "fsyncgate" problem — so retrying is not safe. The operator must restart.
//
// Thread safety: append_set()/append_del() must be serialised by the caller
// (the dispatcher's mutex). The interval-sync thread runs concurrently and only
// touches the file descriptor (fsync is safe alongside write) and atomics.
// healthy() and failure_reason() may be called from any thread.
//
// Exclusive access: the file is locked with flock(); a second server on the
// same data directory fails at startup instead of interleaving records.
class AppendLog {
public:
    // Opens or creates the log, replays it into `store` (which should be empty),
    // repairs a torn tail and leaves the file ready for appends.
    // Throws LogCorruptedError (see above) or std::system_error (I/O errors,
    // lock held by another process).
    static std::unique_ptr<AppendLog> open(const std::filesystem::path& path, KvStore& store,
                                           const LogOptions& options, Logger& logger, RecoveryReport& report);

    ~AppendLog();

    AppendLog(const AppendLog&) = delete;
    AppendLog& operator=(const AppendLog&) = delete;

    // Append one mutation and sync it per policy. Returns false if the log has
    // failed (now or earlier); the caller must then not apply the change.
    [[nodiscard]] bool append_set(std::string_view key, std::string_view value);
    [[nodiscard]] bool append_del(std::string_view key);

    [[nodiscard]] bool healthy() const noexcept { return !failed_.load(); }
    [[nodiscard]] std::string failure_reason() const;
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return size_; }

    // Successful syncs since open(). Lets tests check that each policy syncs
    // when it should — the effect of a sync itself is only observable after a
    // power loss, which no automated test can produce.
    [[nodiscard]] std::uint64_t syncs_performed() const noexcept { return syncs_.load(); }

private:
    AppendLog(posix::UniqueFd fd, std::filesystem::path path, const LogOptions& options, Logger& logger,
              std::uint64_t size);

    bool append_record(const std::string& record);
    void fail(const std::string& reason);
    void sync_if_dirty();
    void interval_sync_loop();

    posix::UniqueFd fd_;
    const std::filesystem::path path_;
    const LogOptions options_;
    Logger& logger_;
    std::uint64_t size_;   // bytes in the file; only changed by append_record (caller-serialised)

    std::atomic<bool> failed_{false};
    mutable std::mutex failure_mutex_;
    std::string failure_reason_;   // guarded by failure_mutex_

    std::atomic<bool> dirty_{false};   // written but not yet synced (interval policy)
    std::atomic<std::uint64_t> syncs_{0};
    std::mutex syncer_mutex_;
    std::condition_variable syncer_cv_;
    bool stopping_ = false;   // guarded by syncer_mutex_
    std::thread syncer_;      // only for SyncPolicy::interval; declared last, started last
};

} // namespace cppkv
