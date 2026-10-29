#pragma once
#include <stddef.h>
#include <stdint.h>

class FirmwareBackend {
 public:
  virtual ~FirmwareBackend() = default;
  virtual bool enterMaintenance() = 0;
  virtual void leaveMaintenance() = 0;
  virtual bool begin(size_t size) = 0;
  virtual size_t write(const uint8_t* data, size_t size) = 0;
  virtual bool finish() = 0;
  virtual void abort() = 0;
};

// Transport-independent upload lifecycle, also exercised by native tests.
class FirmwareUpload {
 public:
  explicit FirmwareUpload(FirmwareBackend& backend) : backend_(backend) {}
  bool start(size_t size, size_t capacity, uint32_t now) {
    if (active_ || installed_) return false;
    error_ = nullptr;
    if (!size || size > capacity) { error_ = "Invalid firmware size or no inactive OTA slot"; return false; }
    held_ = true;
    if (!backend_.enterMaintenance()) return fail("Could not turn all relays off");
    if (!backend_.begin(size)) return fail("Could not start firmware update");
    remaining_ = size;
    started_ = now;
    active_ = true;
    return true;
  }
  bool push(const uint8_t* data, size_t size, uint32_t now) {
    if (!tick(now)) return false;
    if (size > remaining_) return fail("Firmware exceeds Content-Length");
    if (backend_.write(data, size) != size) return fail("Firmware write failed");
    remaining_ -= size;
    return true;
  }
  bool complete(uint32_t now) {
    if (!tick(now)) return false;
    if (remaining_) return fail("Incomplete firmware upload");
    if (!backend_.finish()) return fail("Firmware validation failed");
    active_ = false;
    installed_ = true; // Keep maintenance held until reboot.
    return true;
  }
  void disconnect() { if (active_) fail("Firmware upload disconnected"); }
  bool tick(uint32_t now) {
    if (!active_) return false;
    if (uint32_t(now - started_) >= 120000) return fail("Firmware upload timed out");
    return true;
  }
  bool active() const { return active_; }
  bool installed() const { return installed_; }
  const char* error() const { return error_ ? error_ : "Firmware upload rejected"; }
 private:
  bool fail(const char* error) {
    backend_.abort();
    active_ = false;
    error_ = error;
    if (held_) { backend_.leaveMaintenance(); held_ = false; }
    return false;
  }
  FirmwareBackend& backend_;
  size_t remaining_ = 0;
  uint32_t started_ = 0;
  bool held_ = false, active_ = false, installed_ = false;
  const char* error_ = nullptr;
};
