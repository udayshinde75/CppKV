#include "cppkv/common/logger.hpp"

#include <array>
#include <chrono>
#include <ctime>

namespace cppkv {

namespace {

// UTC timestamp with millisecond precision, e.g. "2026-10-30T08:45:12.345Z".
std::string utc_timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto whole_seconds = time_point_cast<seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - whole_seconds).count();

    const std::time_t time = system_clock::to_time_t(whole_seconds);
    std::tm utc{};
    // gmtime_r is the thread-safe POSIX variant; std::gmtime returns a pointer
    // to shared static storage and is not safe to call from several threads.
    gmtime_r(&time, &utc);

    std::array<char, 32> text{};
    const std::size_t length = std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%S", &utc);

    std::string result{text.data(), length};
    result += '.';
    result += static_cast<char>('0' + millis / 100);
    result += static_cast<char>('0' + millis / 10 % 10);
    result += static_cast<char>('0' + millis % 10);
    result += 'Z';
    return result;
}

} // namespace

std::string_view to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::debug: return "DEBUG";
    case LogLevel::info: return "INFO";
    case LogLevel::warn: return "WARN";
    case LogLevel::error: return "ERROR";
    }
    return "?";   // unreachable for valid enum values; keeps every path returning
}

Logger::Logger(std::ostream& out, LogLevel min_level) : out_{out}, min_level_{min_level} {}

void Logger::log(LogLevel level, std::string_view message) {
    if (!enabled(level)) {
        return;
    }

    // Build the complete line before taking the lock: the critical section is
    // then a single write, so other threads wait as briefly as possible.
    std::string line = utc_timestamp();
    line += ' ';
    const std::string_view level_name = to_string(level);
    line += level_name;
    line.append(6 - level_name.size(), ' ');   // align messages: "INFO  ", "ERROR "
    line += message;
    line += '\n';

    const std::lock_guard lock{mutex_};
    out_ << line;
    out_.flush();
}

} // namespace cppkv
