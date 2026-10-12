#pragma once

#include <cstddef>
#include <cstdint>

namespace kv {

// CRC-32 (IEEE, reflected polynomial 0xEDB88320), the same checksum as zlib's crc32().
// Pass a previous result as `crc` to checksum data in pieces.
uint32_t crc32(const void* data, size_t len, uint32_t crc = 0);

}  // namespace kv
