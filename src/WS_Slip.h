#pragma once
#include <stddef.h>
#include <stdint.h>

// OSC 1.1 double-END SLIP. A returned frame survives until the next feed().
class OscSlipInput {
 public:
  static constexpr size_t capacity = 1024;
  void reset() { used_ = 0; synced_ = escaped_ = bad_ = fragment_ = false; }
  bool expired(uint32_t now) const { return fragment_ && uint32_t(now - started_) >= 2000; }
  const uint8_t* data() const { return buffer_; }
  size_t feed(uint8_t byte, uint32_t now) {
    if (byte == 0xC0) {
      size_t ready = synced_ && !bad_ && !escaped_ ? used_ : 0;
      used_ = 0;
      bad_ = escaped_ = fragment_ = false;
      synced_ = true;
      return ready;
    }
    if (!fragment_) { fragment_ = true; started_ = now; }
    if (!synced_ || bad_) return 0;
    if (escaped_) {
      escaped_ = false;
      if (byte == 0xDC) byte = 0xC0;
      else if (byte == 0xDD) byte = 0xDB;
      else { bad_ = true; return 0; }
    } else if (byte == 0xDB) { escaped_ = true; return 0; }
    if (used_ == capacity) { bad_ = true; return 0; }
    buffer_[used_++] = byte;
    return 0;
  }
 private:
  uint8_t buffer_[capacity]{};
  size_t used_ = 0;
  uint32_t started_ = 0;
  bool synced_ = false, escaped_ = false, bad_ = false, fragment_ = false;
};

// Stream OSC 1.1 SLIP in small chunks rather than placing a worst-case doubled
// packet on the TCP worker's 4 KiB stack. A short write aborts this frame.
template <class Write>
bool OscSlipWrite(const uint8_t* payload, size_t size, Write write) {
  if (!payload || !size || size > OscSlipInput::capacity) return false;
  uint8_t chunk[128];
  size_t used = 0;
  auto flush = [&]() {
    if (!used) return true;
    bool ok = write(chunk, used) == used;
    used = 0;
    return ok;
  };
  auto append = [&](uint8_t byte) {
    chunk[used++] = byte;
    return used < sizeof(chunk) || flush();
  };
  if (!append(0xC0)) return false;
  for (size_t i = 0; i < size; ++i) {
    uint8_t byte = payload[i];
    if (byte == 0xC0 || byte == 0xDB) {
      if (!append(0xDB) || !append(byte == 0xC0 ? 0xDC : 0xDD)) return false;
    } else if (!append(byte)) return false;
  }
  return append(0xC0) && flush();
}
