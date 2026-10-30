#include "cppkv/protocol/line_framer.hpp"

namespace cppkv {

LineFramer::LineFramer(std::size_t max_line_bytes) : max_line_bytes_{max_line_bytes} {}

void LineFramer::feed(std::string_view bytes) {
    if (overflowed_) {
        return;
    }
    // Drop the lines already returned, once per feed() rather than once per
    // line: erasing at the front after every line would move the remaining
    // bytes again and again when many requests arrive in one read.
    // This invalidates views returned by next() — hence the documented rule.
    if (start_ > 0) {
        buffer_.erase(0, start_);
        start_ = 0;
    }
    buffer_.append(bytes);
}

LineFramer::Next LineFramer::next() {
    if (overflowed_) {
        return {Status::too_large, {}};
    }

    const std::string_view pending{buffer_.data() + start_, buffer_.size() - start_};
    const std::size_t newline = pending.find('\n', scanned_);

    if (newline == std::string_view::npos) {
        scanned_ = pending.size();
        if (incomplete_line_too_long(pending)) {
            overflowed_ = true;
            return {Status::too_large, {}};
        }
        return {Status::incomplete, {}};
    }

    std::string_view line = pending.substr(0, newline);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    start_ += newline + 1;
    scanned_ = 0;

    if (line.size() > max_line_bytes_) {
        overflowed_ = true;
        return {Status::too_large, {}};
    }
    return {Status::line, line};
}

bool LineFramer::incomplete_line_too_long(std::string_view pending) const noexcept {
    if (pending.size() <= max_line_bytes_) {
        return false;
    }
    // max_line_bytes + 1 bytes may still become a valid line if the last one
    // is the '\r' of a "\r\n" whose '\n' has not arrived yet.
    return pending.size() > max_line_bytes_ + 1 || pending.back() != '\r';
}

} // namespace cppkv
