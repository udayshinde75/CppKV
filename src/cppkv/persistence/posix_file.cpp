#include "cppkv/persistence/posix_file.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cppkv::posix {

namespace {

std::error_code last_error() noexcept {
    return std::error_code{errno, std::generic_category()};
}

} // namespace

UniqueFd::~UniqueFd() {
    if (fd_ >= 0) {
        // close() errors cannot be handled meaningfully here; durability is
        // established by sync_file(), never by close().
        ::close(fd_);
    }
}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        UniqueFd old{fd_};   // closes the previous descriptor at end of scope
        fd_ = other.release();
    }
    return *this;
}

int UniqueFd::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

std::error_code write_all(int fd, std::string_view bytes) noexcept {
    const char* data = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        const ssize_t written = ::write(fd, data, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;   // interrupted by a signal before writing anything
            }
            return last_error();
        }
        // A short write (written < remaining) is legal, e.g. when a file size
        // limit or a full disk is reached part-way. Continue with the rest;
        // the next call reports the error.
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return {};
}

std::error_code read_exact_at(int fd, std::uint64_t offset, char* out, std::size_t size) noexcept {
    while (size > 0) {
        const ssize_t got = ::pread(fd, out, size, static_cast<off_t>(offset));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return last_error();
        }
        if (got == 0) {
            return std::make_error_code(std::errc::io_error);   // unexpected end of file
        }
        out += got;
        size -= static_cast<std::size_t>(got);
        offset += static_cast<std::uint64_t>(got);
    }
    return {};
}

std::error_code sync_file(int fd) noexcept {
#if defined(__APPLE__)
    if (::fcntl(fd, F_FULLFSYNC) == 0) {
        return {};
    }
    // Some file systems (e.g. network mounts) do not support F_FULLFSYNC.
    // Only then fall back; a real I/O error must be reported, not masked.
    if (errno != ENOTSUP && errno != ENOTTY && errno != EINVAL) {
        return last_error();
    }
    return ::fsync(fd) == 0 ? std::error_code{} : last_error();
#elif defined(__linux__)
    return ::fdatasync(fd) == 0 ? std::error_code{} : last_error();
#else
    return ::fsync(fd) == 0 ? std::error_code{} : last_error();
#endif
}

std::error_code sync_directory(const std::filesystem::path& directory) noexcept {
    const UniqueFd dir{::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    if (!dir.valid()) {
        return last_error();
    }
    return ::fsync(dir.get()) == 0 ? std::error_code{} : last_error();
}

std::error_code file_size(int fd, std::uint64_t& size) noexcept {
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        return last_error();
    }
    size = static_cast<std::uint64_t>(info.st_size);
    return {};
}

std::error_code truncate(int fd, std::uint64_t size) noexcept {
    return ::ftruncate(fd, static_cast<off_t>(size)) == 0 ? std::error_code{} : last_error();
}

} // namespace cppkv::posix
