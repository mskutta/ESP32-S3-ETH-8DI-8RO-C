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
