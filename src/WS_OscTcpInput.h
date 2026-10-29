#pragma once
#include "WS_Slip.h"

// Small transport adapter so the real connection lifecycle can be tested on host.
template <class Server, class Client>
class OscTcpInput {
 public:
  explicit OscTcpInput(uint16_t port) : server_(port) {}
  void stop() {
    for (auto& peer : peers_) { peer.client.stop(); peer.slip.reset(); }
    server_.end();
  }
  void begin(uint16_t port) { stop(); server_ = Server(port); server_.begin(); }
  template <class Receive>
  void poll(uint32_t now, Receive receive) {
    for (auto& peer : peers_) {
      if (!peer.client.connected() || peer.slip.expired(now)) {
        peer.client.stop();
        peer.slip.reset();
      }
    }
    Client incoming = server_.available();
    if (incoming) {
      bool placed = false;
      for (auto& peer : peers_) {
        if (!peer.client) {
          peer.client = incoming;
          peer.slip.reset();
          placed = true;
          break;
        }
      }
      if (!placed) incoming.stop();
    }
    for (auto& peer : peers_) {
      // Bound each client's work so HTTP, timers, and other transports get time.
      for (size_t budget = 0; budget < 512 && peer.client.available(); ++budget) {
        int byte = peer.client.read();
        if (byte < 0) break;
        size_t length = peer.slip.feed(uint8_t(byte), now);
        if (length) receive(peer.slip.data(), length);
      }
    }
  }
 private:
  struct Peer { Client client; OscSlipInput slip; } peers_[2];
  Server server_;
};
