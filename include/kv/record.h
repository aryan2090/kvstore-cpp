#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace kv {

// On-disk record layout (all integers little-endian):
//
//   [crc32: 4][key_len: 4][val_len: 4][flags: 1][key: key_len][value: val_len]
//
// The crc covers every byte after the crc field itself.
inline constexpr size_t kHeaderSize = 13;
inline constexpr uint8_t kFlagTombstone = 0x1;
inline constexpr uint32_t kMaxKeySize = 1u << 16;
inline constexpr uint32_t kMaxValueSize = 64u << 20;

struct RecordHeader {
  uint32_t crc = 0;
  uint32_t key_len = 0;
  uint32_t val_len = 0;
  uint8_t flags = 0;

  size_t record_size() const { return kHeaderSize + key_len + val_len; }
  bool tombstone() const { return flags & kFlagTombstone; }
};

// Appends one encoded record to `out`.
void encode_record(std::string& out, std::string_view key, std::string_view value,
                   bool tombstone);

// Parses the first kHeaderSize bytes of `buf`. Returns false if the fields are
// out of range, which on recovery means garbage rather than a real header.
bool decode_header(const char* buf, RecordHeader& h);

// True if `rec` (h.record_size() bytes, header included) matches its crc.
bool verify_record(const RecordHeader& h, const char* rec);

}  // namespace kv
