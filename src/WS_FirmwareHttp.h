#pragma once
#include <WebServer.h>
#include "WS_FirmwareUpload.h"

class FirmwareWebServer : public WebServer {
 public:
  using WebServer::WebServer;
  // RAW_START consumes the body itself. Skip the core's blocking readBytes loop,
  // which restarts its timeout for every byte and cannot enforce a total deadline.
  void rawBodyConsumed() { _clientContentLength = 0; }
};

class FirmwareHttp : public RequestHandler {
 public:
  FirmwareHttp(FirmwareUpload& upload, size_t (*capacity)(), void (*installed)())
      : upload_(upload), capacity_(capacity), installed_(installed) {}
  bool canHandle(HTTPMethod method, String uri) override {
    bool matches = method == HTTP_PUT && uri == "/api/update";
    if (matches) responded_ = false;
    return matches;
  }
  bool canRaw(String uri) override { return uri == "/api/update"; }
  bool handle(WebServer& server, HTTPMethod, String) override {
    if (!responded_) reject(server, 400, "Use a raw application/octet-stream firmware upload");
    return true;
  }
  void raw(WebServer& server, String, HTTPRaw& raw) override {
    if (raw.status == RAW_START) {
      responded_ = false;
      if (upload_.installed()) { reject(server, 409, "Firmware installed; reboot pending"); return; }
      size_t size = 0, limit = capacity_();
      String length = server.header("Content-Length");
      if (server.header("Content-Type") != "application/octet-stream" ||
          server.hasHeader("Transfer-Encoding") || length.length() == 0) {
        reject(server, 400, "Use application/octet-stream with Content-Length; no transfer encoding");
        return;
      }
      for (size_t i = 0; i < length.length(); ++i) {
        char c = length[i];
        if (c < '0' || c > '9') { reject(server, 400, "Invalid Content-Length"); return; }
        if (size > limit / 10 || (size == limit / 10 && size_t(c - '0') > limit % 10)) {
          reject(server, 413, "Firmware does not fit inactive OTA slot"); return;
        }
        size = size * 10 + size_t(c - '0');
      }
      if (!size || server.clientContentLength() != int(size)) { reject(server, 400, "Invalid Content-Length"); return; }
      static_cast<FirmwareWebServer&>(server).rawBodyConsumed();
      if (!upload_.start(size, limit, millis())) { respond(server, 400); server.client().stop(); return; }
      auto client = server.client();
      uint8_t buffer[512];
      uint32_t lastData = millis();
      size_t remaining = size;
      while (remaining && upload_.tick(millis())) {
        int available = client.available();
        if (available > 0) {
          size_t count = size_t(available);
          if (count > sizeof(buffer)) count = sizeof(buffer);
          if (count > remaining) count = remaining;
          int read = client.read(buffer, count);
          if (read > 0) {
            if (!upload_.push(buffer, size_t(read), millis())) break;
            remaining -= size_t(read);
            lastData = millis();
          }
        } else if (!client.connected() || uint32_t(millis() - lastData) >= 5000) {
          upload_.disconnect();
          break;
        }
        delay(1); // Yield to Ethernet, flash, and hardware-support tasks.
      }
      if (!upload_.active()) { respond(server, 400); client.stop(); }
    } else if (raw.status == RAW_WRITE && upload_.active()) {
      if (!upload_.push(raw.buf, raw.currentSize, millis())) { respond(server, 400); server.client().stop(); }
    } else if (raw.status == RAW_END && upload_.active()) {
      if (upload_.complete(millis())) {
        respond(server, 200);
        installed_();
      } else respond(server, 400);
    } else if (raw.status == RAW_ABORTED) {
      upload_.disconnect();
      if (!responded_) respond(server, 400);
    }
  }
 private:
  void reject(WebServer& server, int code, const char* why) {
    responded_ = true;
    server.send(code, "text/plain", why);
    server.client().stop();
  }
  void respond(WebServer& server, int code) {
    responded_ = true;
    server.send(code, "application/json", code == 200 ? "{\"installed\":true,\"rebooting\":true}" :
                String("{\"error\":\"") + upload_.error() + "\"}");
  }
  FirmwareUpload& upload_;
  size_t (*capacity_)();
  void (*installed_)();
  bool responded_ = false;
};
