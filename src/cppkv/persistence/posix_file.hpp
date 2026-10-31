#pragma once

// Thin RAII and error-handling helpers over POSIX file descriptors.
//
// Why POSIX and not std::fstream: durable storage needs fsync() (and
// fcntl(F_FULLFSYNC) on macOS), file locking and ftruncate(), none of which
// the standard library offers. These helpers keep the system calls in one
// place and report errors as std::error_code values.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace cppkv::posix {

// Owns a file descriptor and closes it exactly once (RAII). Move-only:
// copying would lead to a double close().
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_{fd} {}
    ~UniqueFd();

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_{other.release()} {}
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept;

private:
    int fd_ = -1;
};

// Writes all bytes, retrying after partial writes and EINTR.
[[nodiscard]] std::error_code write_all(int fd, std::string_view bytes) noexcept;

// Reads exactly `size` bytes at `offset` into `out` (pread, so the file
// position is not used). Returns an error, including on unexpected EOF.
[[nodiscard]] std::error_code read_exact_at(int fd, std::uint64_t offset, char* out, std::size_t size) noexcept;

// Makes the file's written data durable.
// - macOS: fcntl(F_FULLFSYNC). Plain fsync() on macOS only passes data to the
//   drive, which may keep it in its volatile cache; F_FULLFSYNC asks the drive
//   to flush that cache. Falls back to fsync() where F_FULLFSYNC is not
//   supported by the file system.
// - Linux: fdatasync() — flushes data plus the metadata needed to read it back
//   (such as the file size), but skips timestamps.
[[nodiscard]] std::error_code sync_file(int fd) noexcept;

// Makes a directory's entries durable, i.e. that a newly created file exists
// after a power loss. Needed once after creating the log file.
[[nodiscard]] std::error_code sync_directory(const std::filesystem::path& directory) noexcept;

[[nodiscard]] std::error_code file_size(int fd, std::uint64_t& size) noexcept;
[[nodiscard]] std::error_code truncate(int fd, std::uint64_t size) noexcept;

} // namespace cppkv::posix
