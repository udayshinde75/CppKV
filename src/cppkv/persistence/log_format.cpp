#include "cppkv/persistence/log_format.hpp"

#include <boost/crc.hpp>

#include <stdexcept>

namespace cppkv::log_format {

namespace {

void append_u16(std::string& out, std::uint16_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8U) & 0xFFU);
}

void append_u32(std::string& out, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        out += static_cast<char>((value >> shift) & 0xFFU);
    }
}

std::uint32_t checked_length(std::size_t size) {
    if (size > kMaxPayloadBytes) {
        throw std::length_error{"log record payload exceeds the maximum size"};
    }
    return static_cast<std::uint32_t>(size);
}

// Wraps a payload in the record header: length, then CRC of the payload.
std::string make_record(const std::string& payload) {
    std::string record;
    record.reserve(kRecordHeaderBytes + payload.size());
    append_u32(record, checked_length(payload.size()));
    append_u32(record, crc32(payload));
    record += payload;
    return record;
}

} // namespace

std::string encode_header() {
    std::string header{kMagic};
    append_u16(header, kVersion);
    return header;
}

std::string encode_set(std::string_view key, std::string_view value) {
    std::string payload;
    payload.reserve(1 + 4 + key.size() + 4 + value.size());
    payload += static_cast<char>(Op::set);
    append_u32(payload, checked_length(key.size()));
    payload += key;
    append_u32(payload, checked_length(value.size()));
    payload += value;
    return make_record(payload);
}

std::string encode_del(std::string_view key) {
    std::string payload;
    payload.reserve(1 + 4 + key.size());
    payload += static_cast<char>(Op::del);
    append_u32(payload, checked_length(key.size()));
    payload += key;
    return make_record(payload);
}

std::uint32_t crc32(std::string_view bytes) noexcept {
    boost::crc_32_type crc;
    crc.process_bytes(bytes.data(), bytes.size());
    return crc.checksum();
}

std::uint32_t read_u32(std::string_view bytes) noexcept {
    // Bytes are read as unsigned char: with a signed char, 0xFF would become
    // -1 and sign-extend into the upper bits of the result.
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[i])) << (8U * i);
    }
    return value;
}

std::optional<Record> decode_payload(std::string_view payload) {
    // A reader over `payload` that refuses to go past its end.
    std::size_t pos = 0;
    const auto take = [&](std::size_t n) -> std::optional<std::string_view> {
        if (payload.size() - pos < n) {
            return std::nullopt;
        }
        const std::string_view part = payload.substr(pos, n);
        pos += n;
        return part;
    };
    const auto take_u32 = [&]() -> std::optional<std::uint32_t> {
        const auto bytes = take(4);
        return bytes ? std::optional{read_u32(*bytes)} : std::nullopt;
    };

    const auto op_byte = take(1);
    if (!op_byte) {
        return std::nullopt;
    }
    const auto op = static_cast<unsigned char>((*op_byte)[0]);
    if (op != static_cast<unsigned char>(Op::set) && op != static_cast<unsigned char>(Op::del)) {
        return std::nullopt;
    }

    Record record;
    record.op = static_cast<Op>(op);

    const auto key_length = take_u32();
    const auto key = key_length ? take(*key_length) : std::nullopt;
    if (!key) {
        return std::nullopt;
    }
    record.key = std::string{*key};

    if (record.op == Op::set) {
        const auto value_length = take_u32();
        const auto value = value_length ? take(*value_length) : std::nullopt;
        if (!value) {
            return std::nullopt;
        }
        record.value = std::string{*value};
    }

    if (pos != payload.size()) {
        return std::nullopt;   // trailing bytes: the lengths do not describe the payload
    }
    return record;
}

} // namespace cppkv::log_format
