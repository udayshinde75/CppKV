#pragma once

#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

namespace cppkv {

enum class LogLevel { debug, info, warn, error };

[[nodiscard]] std::string_view to_string(LogLevel level) noexcept;

// Minimal levelled logger writing one line per message:
//
//   2026-10-30T08:45:12.345Z INFO  client connected peer=127.0.0.1:54321
//
// Thread safety: log() may be called from any number of threads at once.
// A mutex serialises writes so that lines from different threads never
// interleave character by character.
//
// Lifetime: the logger stores a reference to `out`; the stream must outlive
// the logger. In the server, `out` is std::cerr (lives for the whole program);
// in tests it is a std::ostringstream declared before the logger.
class Logger {
public:
    explicit Logger(std::ostream& out, LogLevel min_level = LogLevel::info);

    // Messages below min_level are dropped without formatting.
    void log(LogLevel level, std::string_view message);

    void debug(std::string_view message) { log(LogLevel::debug, message); }
    void info(std::string_view message) { log(LogLevel::info, message); }
    void warn(std::string_view message) { log(LogLevel::warn, message); }
    void error(std::string_view message) { log(LogLevel::error, message); }

    [[nodiscard]] bool enabled(LogLevel level) const noexcept { return level >= min_level_; }

private:
    std::mutex mutex_;   // guards out_; also makes Logger non-copyable and non-movable
    std::ostream& out_;
    const LogLevel min_level_;
};

} // namespace cppkv
