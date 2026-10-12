#include "kv/crc32.h"

#include <array>

namespace kv {
namespace {

constexpr std::array<uint32_t, 256> make_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    }
    table[i] = c;
  }
  return table;
}

constexpr auto kTable = make_table();

}  // namespace

uint32_t crc32(const void* data, size_t len, uint32_t crc) {
  const auto* p = static_cast<const unsigned char*>(data);
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc = kTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  }
  return ~crc;
}

}  // namespace kv
