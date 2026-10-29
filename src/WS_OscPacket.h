#pragma once
#include <stddef.h>
#include <stdint.h>

// Validate bounds before ArduinoOSC walks strings and blob lengths.
inline bool OscPacketValid(const uint8_t* data, size_t length) {
  if (!length || length % 4 || data[0] != '/') return false;
  size_t offset = 0;
  auto string = [&]() {
    while (offset < length && data[offset]) ++offset;
    if (offset == length) return false;
    do { if (data[offset++]) return false; } while (offset % 4 && offset < length);
    return offset <= length;
  };
  if (!string() || offset == length || data[offset] != ',') return false;
  size_t tags = offset + 1;
  if (!string()) return false;
  for (size_t i = tags; data[i]; ++i) {
    size_t size = 0;
    switch (data[i]) {
      case 'T': case 'F': break;
      case 'i': case 'f': size = 4; break;
      case 'h': case 'd': size = 8; break;
      case 's': if (!string()) return false; continue;
      case 'b': {
        if (length - offset < 4) return false;
        uint32_t bytes = (uint32_t(data[offset]) << 24) | (uint32_t(data[offset+1]) << 16) |
                         (uint32_t(data[offset+2]) << 8) | data[offset+3];
        offset += 4;
        if (bytes > length - offset) return false;
        size = (size_t(bytes) + 3) & ~size_t(3);
        break;
      }
      default: return false;
    }
    if (size > length - offset) return false;
    offset += size;
  }
  return offset == length;
}
