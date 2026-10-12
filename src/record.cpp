#include "kv/record.h"

#include "kv/crc32.h"

namespace kv {
namespace {

void put_u32(char* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

uint32_t get_u32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= uint32_t{static_cast<unsigned char>(p[i])} << (8 * i);
  return v;
}

}  // namespace

void encode_record(std::string& out, std::string_view key, std::string_view value,
                   bool tombstone) {
  const size_t start = out.size();
  out.resize(start + kHeaderSize);
  char* h = out.data() + start;
  put_u32(h + 4, static_cast<uint32_t>(key.size()));
  put_u32(h + 8, static_cast<uint32_t>(value.size()));
  h[12] = static_cast<char>(tombstone ? kFlagTombstone : 0);
  out.append(key);
  out.append(value);
  const char* rec = out.data() + start;  // re-read: append may have reallocated
  const uint32_t crc = crc32(rec + 4, out.size() - start - 4);
  put_u32(out.data() + start, crc);
}

bool decode_header(const char* buf, RecordHeader& h) {
  h.crc = get_u32(buf);
  h.key_len = get_u32(buf + 4);
  h.val_len = get_u32(buf + 8);
  h.flags = static_cast<uint8_t>(buf[12]);
  return h.key_len <= kMaxKeySize && h.val_len <= kMaxValueSize &&
         (h.flags & ~kFlagTombstone) == 0;
}

bool verify_record(const RecordHeader& h, const char* rec) {
  return crc32(rec + 4, h.record_size() - 4) == h.crc;
}

}  // namespace kv
