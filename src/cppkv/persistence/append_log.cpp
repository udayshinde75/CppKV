#include "cppkv/persistence/append_log.hpp"

#include "cppkv/persistence/log_format.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <optional>
#include <system_error>
#include <utility>

namespace cppkv {

namespace fmt = log_format;

namespace {

[[noreturn]] void throw_io(const std::error_code& ec, const std::string& what) {
    throw std::system_error{ec, what};
}

// Replays records from an open log file into a store and decides what to do
// with damage. Holds no state beyond one pass over the file.
class Replayer {
public:
    Replayer(int fd, std::uint64_t file_size, KvStore& store) : fd_{fd}, file_size_{file_size}, store_{store} {}

    enum class Outcome { clean, torn_tail, corrupt };

    struct Result {
        Outcome outcome = Outcome::clean;
        std::uint64_t valid_end = 0;   // offset just after the last good record
        std::uint64_t records = 0;
        std::string reason;
    };

    Result run() {
        Result result;
        std::uint64_t offset = fmt::kHeaderBytes;

        while (offset < file_size_) {
            const std::uint64_t remaining = file_size_ - offset;
            const auto damaged = [&](Outcome outcome, std::string reason) {
                result.outcome = outcome;
                result.reason = std::move(reason) + " at offset " + std::to_string(offset);
                result.valid_end = offset;
                return result;
            };

            // Rule 1: not even a complete record header -> a write was cut short.
            if (remaining < fmt::kRecordHeaderBytes) {
                return damaged(Outcome::torn_tail, "incomplete record header");
            }

            std::array<char, fmt::kRecordHeaderBytes> header{};
            read_at(offset, header.data(), header.size());
            const std::uint32_t length = fmt::read_u32({header.data(), 4});
            const std::uint32_t expected_crc = fmt::read_u32({header.data() + 4, 4});

            // Rule 2: an absurd length is never produced by a torn write (which
            // loses bytes but does not invent them) — unless the "length" is
            // part of a zero-filled tail.
            if (length > fmt::kMaxPayloadBytes) {
                return rest_is_zero(offset) ? damaged(Outcome::torn_tail, "zero-filled tail")
                                            : damaged(Outcome::corrupt, "record length " + std::to_string(length) +
                                                                            " exceeds the maximum");
            }

            // Rule 3: the record claims more bytes than the file has -> cut short.
            if (fmt::kRecordHeaderBytes + length > remaining) {
                return damaged(Outcome::torn_tail, "record extends past end of file");
            }

            std::string payload(length, '\0');
            read_at(offset + fmt::kRecordHeaderBytes, payload.data(), payload.size());

            std::optional<fmt::Record> record;
            if (fmt::crc32(payload) == expected_crc) {
                record = fmt::decode_payload(payload);
            }

            // Rule 4: a complete but damaged record. If it is the last thing in
            // the file, or only zeros follow, it is what a crash leaves behind.
            // If real data follows, the damage is in the middle of the log.
            if (!record) {
                const std::uint64_t end = offset + fmt::kRecordHeaderBytes + length;
                const std::string what = fmt::crc32(payload) == expected_crc ? "malformed record" : "checksum mismatch";
                if (end == file_size_) {
                    return damaged(Outcome::torn_tail, what + " in last record");
                }
                if (rest_is_zero(offset)) {
                    return damaged(Outcome::torn_tail, "zero-filled tail");
                }
                return damaged(Outcome::corrupt, what + ", followed by " + std::to_string(file_size_ - end) +
                                                     " more byte(s)");
            }

            apply(*record);
            ++result.records;
            offset += fmt::kRecordHeaderBytes + length;
        }

        result.valid_end = offset;
        return result;
    }

private:
    void read_at(std::uint64_t offset, char* out, std::size_t size) const {
        if (const auto ec = posix::read_exact_at(fd_, offset, out, size)) {
            throw_io(ec, "reading log at offset " + std::to_string(offset));
        }
    }

    // True if every byte from `offset` to the end of the file is zero. File
    // systems may extend a file's size before its data reaches the disk; after
    // a power loss that region reads back as zeros.
    [[nodiscard]] bool rest_is_zero(std::uint64_t offset) const {
        std::array<char, 4096> chunk{};
        while (offset < file_size_) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), file_size_ - offset));
            read_at(offset, chunk.data(), n);
            for (std::size_t i = 0; i < n; ++i) {
                if (chunk[i] != '\0') {
                    return false;
                }
            }
            offset += n;
        }
        return true;
    }

    void apply(fmt::Record& record) {
        if (record.op == fmt::Op::set) {
            store_.set(std::move(record.key), std::move(record.value));
        } else {
            store_.erase(record.key);
        }
    }

    int fd_;
    std::uint64_t file_size_;
    KvStore& store_;
};

} // namespace

std::unique_ptr<AppendLog> AppendLog::open(const std::filesystem::path& path, KvStore& store,
                                           const LogOptions& options, Logger& logger, RecoveryReport& report) {
    report = RecoveryReport{};

    // O_APPEND: every write goes to the current end of file atomically.
    // O_CLOEXEC: the descriptor is not inherited by child processes.
    posix::UniqueFd fd{::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644)};
    if (!fd.valid()) {
        throw_io({errno, std::generic_category()}, "opening log " + path.string());
    }

    // One server per log. flock() is released automatically when the
    // descriptor is closed, including when the process dies.
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        const std::error_code ec{errno, std::generic_category()};
        if (ec == std::errc::operation_would_block) {
            throw std::system_error{ec, "log " + path.string() + " is in use by another process"};
        }
        throw_io(ec, "locking log " + path.string());
    }

    std::uint64_t size = 0;
    if (const auto ec = posix::file_size(fd.get(), size)) {
        throw_io(ec, "reading size of log " + path.string());
    }

    const std::string expected_header = fmt::encode_header();

    // --- Header ------------------------------------------------------------
    bool write_header = false;
    if (size < fmt::kHeaderBytes) {
        // Empty file, or a crash while writing the header of a new log. Only a
        // prefix of our own header may be overwritten; anything else might be
        // someone else's file.
        std::string existing(static_cast<std::size_t>(size), '\0');
        if (size > 0) {
            if (const auto ec = posix::read_exact_at(fd.get(), 0, existing.data(), existing.size())) {
                throw_io(ec, "reading log header");
            }
        }
        if (expected_header.compare(0, existing.size(), existing) != 0) {
            throw LogCorruptedError{path.string() + " is not a CppKV log (bad header)", 0};
        }
        write_header = true;
    } else {
        std::string header(fmt::kHeaderBytes, '\0');
        if (const auto ec = posix::read_exact_at(fd.get(), 0, header.data(), header.size())) {
            throw_io(ec, "reading log header");
        }
        if (header.compare(0, fmt::kMagic.size(), fmt::kMagic) != 0) {
            throw LogCorruptedError{path.string() + " is not a CppKV log (bad magic)", 0};
        }
        if (header != expected_header) {
            throw LogCorruptedError{path.string() + " has an unsupported log format version", 0};
        }
    }

    if (write_header) {
        if (const auto ec = posix::truncate(fd.get(), 0)) {
            throw_io(ec, "initialising log");
        }
        if (const auto ec = posix::write_all(fd.get(), expected_header)) {
            throw_io(ec, "writing log header");
        }
        // Durable creation: the header itself, and the directory entry that
        // makes the file findable after a power loss.
        if (const auto ec = posix::sync_file(fd.get())) {
            throw_io(ec, "syncing new log");
        }
        const auto directory = path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
        if (const auto ec = posix::sync_directory(directory)) {
            throw_io(ec, "syncing directory " + directory.string());
        }
        report.created = true;
        report.valid_bytes = fmt::kHeaderBytes;
        logger.info("initialised new log " + path.string());
        return std::unique_ptr<AppendLog>{new AppendLog{std::move(fd), path, options, logger, fmt::kHeaderBytes}};
    }

    // --- Records -----------------------------------------------------------
    Replayer replayer{fd.get(), size, store};
    const Replayer::Result result = replayer.run();
    report.records_applied = result.records;
    report.valid_bytes = result.valid_end;

    if (result.outcome == Replayer::Outcome::corrupt && options.recovery == RecoveryMode::strict) {
        throw LogCorruptedError{"log " + path.string() + " is corrupted: " + result.reason +
                                    ". Refusing to start; inspect the file, or restart with --recovery truncate"
                                    " to keep the first " + std::to_string(result.valid_end) + " byte(s)",
                                result.valid_end};
    }

    if (result.outcome != Replayer::Outcome::clean) {
        report.discarded_bytes = size - result.valid_end;
        report.discard_reason = result.reason;
        if (const auto ec = posix::truncate(fd.get(), result.valid_end)) {
            throw_io(ec, "truncating damaged log tail");
        }
        if (const auto ec = posix::sync_file(fd.get())) {
            throw_io(ec, "syncing repaired log");
        }
        logger.warn("log " + path.string() + ": discarded " + std::to_string(report.discarded_bytes) +
                    " byte(s) (" + result.reason + ")");
    }

    logger.info("recovered " + std::to_string(result.records) + " record(s) from " + path.string());
    return std::unique_ptr<AppendLog>{new AppendLog{std::move(fd), path, options, logger, result.valid_end}};
}

AppendLog::AppendLog(posix::UniqueFd fd, std::filesystem::path path, const LogOptions& options, Logger& logger,
                     std::uint64_t size)
    : fd_{std::move(fd)}, path_{std::move(path)}, options_{options}, logger_{logger}, size_{size} {
    if (options_.sync == SyncPolicy::interval) {
        // Started last, after every member it uses is initialised.
        syncer_ = std::thread{[this] { interval_sync_loop(); }};
    }
}

AppendLog::~AppendLog() {
    if (syncer_.joinable()) {
        {
            const std::lock_guard lock{syncer_mutex_};
            stopping_ = true;
        }
        syncer_cv_.notify_all();
        syncer_.join();
    }
    // On a clean shutdown, make the interval policy's last writes durable too.
    if (options_.sync == SyncPolicy::interval) {
        sync_if_dirty();
    }
}

bool AppendLog::append_set(std::string_view key, std::string_view value) {
    return append_record(fmt::encode_set(key, value));
}

bool AppendLog::append_del(std::string_view key) {
    return append_record(fmt::encode_del(key));
}

bool AppendLog::append_record(const std::string& record) {
    if (failed_.load()) {
        return false;
    }

    if (const auto ec = posix::write_all(fd_.get(), record)) {
        // Part of the record may have reached the file. Cut it off, so the
        // file ends with a complete record again; then stop accepting writes.
        const auto truncate_error = posix::truncate(fd_.get(), size_);
        fail("write failed: " + ec.message() +
             (truncate_error ? "; rollback failed: " + truncate_error.message() : std::string{}));
        return false;
    }
    size_ += record.size();

    switch (options_.sync) {
    case SyncPolicy::always:
        if (const auto ec = posix::sync_file(fd_.get())) {
            fail("sync failed: " + ec.message());
            return false;
        }
        ++syncs_;
        break;
    case SyncPolicy::interval:
        dirty_.store(true);
        break;
    case SyncPolicy::never:
        break;
    }
    return true;
}

void AppendLog::fail(const std::string& reason) {
    {
        const std::lock_guard lock{failure_mutex_};
        if (failed_.load()) {
            return;   // keep the first reason
        }
        failure_reason_ = reason;
        failed_.store(true);
    }
    logger_.error("log " + path_.string() + " failed: " + reason + "; refusing further writes until restart");
}

std::string AppendLog::failure_reason() const {
    const std::lock_guard lock{failure_mutex_};
    return failure_reason_;
}

void AppendLog::sync_if_dirty() {
    if (!dirty_.exchange(false) || failed_.load()) {
        return;
    }
    if (const auto ec = posix::sync_file(fd_.get())) {
        fail("sync failed: " + ec.message());
        return;
    }
    ++syncs_;
}

void AppendLog::interval_sync_loop() {
    std::unique_lock lock{syncer_mutex_};
    while (!stopping_) {
        // wait_for returns early (true) when stopping_ is set by the destructor.
        if (syncer_cv_.wait_for(lock, options_.sync_interval, [this] { return stopping_; })) {
            break;
        }
        lock.unlock();   // never hold the mutex during the (slow) fsync
        sync_if_dirty();
        lock.lock();
    }
}

} // namespace cppkv
