#pragma once

// On-disk format of the CppKV append-only log (docs/adr/0003-persistence.md).
// Pure functions: no file I/O, so the format is unit-testable byte by byte.
//
//   file    := header record*
//   header  := "CPPKVLOG" u16:version                         (10 bytes)
//   record  := u32:payload_len u32:crc32(payload) payload     (8 + payload_len bytes)
//   payload := u8:op u32:key_len key [u32:value_len value]    (value only for SET)
//   op      := 1 (SET) | 2 (DEL)
//
// All integers are unsigned little-endian, written byte by byte with shifts.
// The format therefore does not depend on the host's endianness, struct
// padding or char signedness, so a log written on an arm64 Mac can be read
// on an x86-64 Linux machine and vice versa.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cppkv::log_format {

inline constexpr std::string_view kMagic = "CPPKVLOG";
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kHeaderBytes = 10;         // magic + version
inline constexpr std::size_t kRecordHeaderBytes = 8;    // payload_len + crc32

// Upper bound for one record's payload. Protocol limits keep real records far
// below this (key <= 512 bytes, request line <= 64 KiB). The bound exists so
// that a corrupted length field (e.g. 0xFFFFFFFF) is recognised as corruption
// instead of triggering a 4 GiB allocation during recovery.
inline constexpr std::size_t kMaxPayloadBytes = std::size_t{16} * 1024 * 1024;

enum class Op : std::uint8_t { set = 1, del = 2 };

struct Record {
    Op op = Op::set;
    std::string key;
    std::string value;   // empty for DEL

    friend bool operator==(const Record&, const Record&) = default;
};

// The 10-byte file header.
[[nodiscard]] std::string encode_header();

// A complete record (record header + payload), ready to append.
// Throws std::length_error if the payload would exceed kMaxPayloadBytes.
[[nodiscard]] std::string encode_set(std::string_view key, std::string_view value);
[[nodiscard]] std::string encode_del(std::string_view key);

// Standard CRC-32 (IEEE 802.3, as used by zlib and PNG): crc32("123456789") == 0xCBF43926.
[[nodiscard]] std::uint32_t crc32(std::string_view bytes) noexcept;

// Reads a little-endian u32 from the first 4 bytes of `bytes` (size >= 4).
[[nodiscard]] std::uint32_t read_u32(std::string_view bytes) noexcept;

// Parses a payload whose CRC has already been checked. Returns nullopt if the
// payload is structurally invalid: unknown op, a length pointing past the end,
// or trailing bytes.
[[nodiscard]] std::optional<Record> decode_payload(std::string_view payload);

} // namespace cppkv::log_format
