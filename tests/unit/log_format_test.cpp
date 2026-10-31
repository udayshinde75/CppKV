#include "cppkv/persistence/log_format.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace fmt = cppkv::log_format;
using namespace std::string_literals;

namespace {

std::string payload_of(const std::string& record) {
    return record.substr(fmt::kRecordHeaderBytes);
}

TEST(LogFormat, HeaderBytes) {
    EXPECT_EQ(fmt::encode_header(), "CPPKVLOG\x01\x00"s);
    EXPECT_EQ(fmt::encode_header().size(), fmt::kHeaderBytes);
}

TEST(LogFormat, Crc32CheckValue) {
    // The standard check value of CRC-32/ISO-HDLC (zlib, PNG, Ethernet).
    EXPECT_EQ(fmt::crc32("123456789"), 0xCBF43926U);
    EXPECT_EQ(fmt::crc32(""), 0U);
}

// Golden bytes, computed independently with Python's zlib.crc32. If one of
// these fails, the on-disk format changed and existing logs would become
// unreadable: bump kVersion instead.
TEST(LogFormat, GoldenSetRecord) {
    EXPECT_EQ(fmt::encode_set("k", "v"),
              "\x0b\x00\x00\x00"   // payload length 11
              "\x32\x07\xfe\xa3"   // crc32 0xa3fe0732, little-endian
              "\x01"               // op SET
              "\x01\x00\x00\x00" "k"
              "\x01\x00\x00\x00" "v"s);
}

TEST(LogFormat, GoldenDelRecord) {
    EXPECT_EQ(fmt::encode_del("k"),
              "\x06\x00\x00\x00"
              "\xc8\x91\x0a\x1b"   // crc32 0x1b0a91c8
              "\x02"
              "\x01\x00\x00\x00" "k"s);
}

TEST(LogFormat, ReadU32IsLittleEndianAndUnsigned) {
    EXPECT_EQ(fmt::read_u32("\x01\x02\x03\x04"s), 0x04030201U);
    EXPECT_EQ(fmt::read_u32("\xff\xff\xff\xff"s), 0xFFFFFFFFU);   // no sign extension
}

TEST(LogFormat, RoundTrip) {
    const fmt::Record set{fmt::Op::set, "city", "Frankfurt am Main"};
    EXPECT_EQ(fmt::decode_payload(payload_of(fmt::encode_set(set.key, set.value))), set);

    const fmt::Record empty_value{fmt::Op::set, "k", ""};
    EXPECT_EQ(fmt::decode_payload(payload_of(fmt::encode_set("k", ""))), empty_value);

    const fmt::Record binary{fmt::Op::set, "a\0b"s, "\r\n\0\xff"s};
    EXPECT_EQ(fmt::decode_payload(payload_of(fmt::encode_set(binary.key, binary.value))), binary);

    const fmt::Record del{fmt::Op::del, "gone", ""};
    EXPECT_EQ(fmt::decode_payload(payload_of(fmt::encode_del("gone"))), del);
}

TEST(LogFormat, LengthAndCrcFieldsDescribeThePayload) {
    const std::string record = fmt::encode_set("key", "value");
    const std::string payload = payload_of(record);
    EXPECT_EQ(fmt::read_u32(record), payload.size());
    EXPECT_EQ(fmt::read_u32(record.substr(4)), fmt::crc32(payload));
}

TEST(LogFormat, DecodeRejectsMalformedPayloads) {
    const std::string good = payload_of(fmt::encode_set("k", "v"));
    EXPECT_FALSE(fmt::decode_payload(""));
    EXPECT_FALSE(fmt::decode_payload("\x03" + good.substr(1)));          // unknown op
    EXPECT_FALSE(fmt::decode_payload(good.substr(0, good.size() - 1)));  // value cut short
    EXPECT_FALSE(fmt::decode_payload(good + "x"));                       // trailing byte
    EXPECT_FALSE(fmt::decode_payload("\x01\xff\xff\xff\xff" "k"s));      // key length past end
    EXPECT_FALSE(fmt::decode_payload(payload_of(fmt::encode_del("k")) + "\x00"s));
}

TEST(LogFormat, RefusesPayloadsAboveTheMaximum) {
    const std::string huge(fmt::kMaxPayloadBytes, 'x');
    EXPECT_THROW((void)fmt::encode_set("k", huge), std::length_error);
}

} // namespace
