#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cppkv {

// Splits a TCP byte stream into request lines. Pure: no sockets.
//
// TCP preserves byte order but not message boundaries. A client calling
// send("SET name hello\nGET name\n") once may be received as
//   "SE" | "T name hel" | "lo\nGET name\n"
// The framer accumulates bytes from feed() and returns each complete line
// from next(), keeping any incomplete tail for later.
//
// Rules (docs/protocol.md):
// - A line ends with '\n'. One '\r' directly before the '\n' is removed.
// - A line longer than max_line_bytes (excluding the terminator) is an error.
//   It is detected as early as possible: once max_line_bytes + 1 bytes are
//   buffered without '\n' (one extra byte is allowed if it is the '\r' of a
//   "\r\n" still to come). After that the framer stays in the too_large state:
//   the stream cannot be resynchronised reliably.
//
// Memory: at most max_line_bytes + 1 bytes of an incomplete line plus the
// bytes of the most recent feed() are buffered.
//
// Cost: each byte is scanned for '\n' once, even if a line arrives one byte
// at a time (a naive rescan of the whole buffer per feed() would be quadratic,
// a cheap CPU attack by a slow client).
class LineFramer {
public:
    enum class Status { line, incomplete, too_large };

    struct Next {
        Status status = Status::incomplete;
        // When status == line: the line without its terminator. A view into
        // the framer's buffer, valid until the next call to feed().
        std::string_view line;
    };

    explicit LineFramer(std::size_t max_line_bytes);

    // Appends received bytes. Ignored once the framer is in the too_large state.
    void feed(std::string_view bytes);

    // Returns the next complete line, or incomplete / too_large.
    [[nodiscard]] Next next();

    // Bytes received but not yet returned as a line.
    [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffer_.size() - start_; }

private:
    [[nodiscard]] bool incomplete_line_too_long(std::string_view pending) const noexcept;

    std::size_t max_line_bytes_;
    std::string buffer_;
    std::size_t start_ = 0;     // first byte not yet returned
    std::size_t scanned_ = 0;   // bytes after start_ already known to contain no '\n'
    bool overflowed_ = false;
};

} // namespace cppkv
