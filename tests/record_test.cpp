#include <gtest/gtest.h>

#include <string>

#include "kv/crc32.h"
#include "kv/record.h"

namespace kv {
namespace {

TEST(Crc32, MatchesKnownVectors) {
  // Standard check value for CRC-32/IEEE.
  EXPECT_EQ(crc32("123456789", 9), 0xCBF43926u);
  EXPECT_EQ(crc32("", 0), 0u);
  EXPECT_EQ(crc32("The quick brown fox jumps over the lazy dog", 43), 0x414FA339u);
}

TEST(Crc32, IncrementalEqualsOneShot) {
  const std::string s = "hello, incremental world";
  const uint32_t part = crc32(s.data(), 10);
  EXPECT_EQ(crc32(s.data() + 10, s.size() - 10, part), crc32(s.data(), s.size()));
}

TEST(Record, EncodeDecodeRoundTrip) {
  std::string buf = "prefix";  // encode appends; the record must not depend on earlier bytes
  encode_record(buf, "key", "value", false);
  const char* rec = buf.data() + 6;
  RecordHeader h;
  ASSERT_TRUE(decode_header(rec, h));
  EXPECT_EQ(h.key_len, 3u);
  EXPECT_EQ(h.val_len, 5u);
  EXPECT_FALSE(h.tombstone());
  EXPECT_EQ(h.record_size(), buf.size() - 6);
  EXPECT_TRUE(verify_record(h, rec));
  EXPECT_EQ(std::string(rec + kHeaderSize, 3), "key");
  EXPECT_EQ(std::string(rec + kHeaderSize + 3, 5), "value");
}

TEST(Record, TombstoneFlag) {
  std::string buf;
  encode_record(buf, "k", "", true);
  RecordHeader h;
  ASSERT_TRUE(decode_header(buf.data(), h));
  EXPECT_TRUE(h.tombstone());
  EXPECT_TRUE(verify_record(h, buf.data()));
}

TEST(Record, EveryFlippedByteIsDetected) {
  std::string buf;
  encode_record(buf, "some-key", "some-value", false);
  for (size_t i = 0; i < buf.size(); ++i) {
    std::string bad = buf;
    bad[i] ^= 0x01;
    RecordHeader h;
    // Either the header fails range checks, the length no longer matches, or the crc fails.
    const bool accepted = decode_header(bad.data(), h) && h.record_size() == bad.size() &&
                          verify_record(h, bad.data());
    EXPECT_FALSE(accepted) << "flip at byte " << i << " went undetected";
  }
}

TEST(Record, RejectsOversizedLengths) {
  std::string buf(kHeaderSize, '\xff');
  RecordHeader h;
  EXPECT_FALSE(decode_header(buf.data(), h));
}

}  // namespace
}  // namespace kv
