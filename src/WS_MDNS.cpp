#include "WS_MDNS.h"

#include <ESPmDNS.h>

#include "WS_ETH.h"

namespace {

constexpr uint32_t kQlabQueryIntervalMs = 5000;
constexpr size_t kMaxQlabTargets = 8;
constexpr uint32_t kQlabStaleTimeoutMs = 30000;

String gHostname = "esp32-eth0";
uint16_t gOscTcpPort = 53000;
bool gMdnsStarted = false;
unsigned long gLastQueryMs = 0;
IPAddress gQlabIps[kMaxQlabTargets];
uint16_t gQlabPorts[kMaxQlabTargets] = {0};
size_t gQlabCount = 0;
unsigned long gLastResolvedQlabMs = 0;

void clearQlabTargets() {
  gQlabCount = 0;
  gLastResolvedQlabMs = 0;
  for (size_t i = 0; i < kMaxQlabTargets; ++i) {
    gQlabIps[i] = IPAddress();
    gQlabPorts[i] = 0;
  }
}

void stopMdns() {
  if (!gMdnsStarted) {
    return;
  }

  MDNS.end();
  gMdnsStarted = false;
  gLastQueryMs = 0;
  clearQlabTargets();
  Serial.println("mDNS stopped");
}

void startMdns() {
  if (gMdnsStarted || !ETH_Connected()) {
    return;
  }

  if (!MDNS.begin(gHostname.c_str())) {
    Serial.printf("mDNS start failed for %s.local\n", gHostname.c_str());
    return;
  }

  MDNS.setInstanceName(gHostname);
  MDNS.addService("osc", "tcp", gOscTcpPort);
  MDNS.addServiceTxt("osc", "tcp", "transport", "tcp");
  MDNS.addServiceTxt("osc", "tcp", "path", "/relay/*");
  MDNS.addServiceTxt("osc", "tcp", "board", "ESP32-S3-ETH-8DI-8RO-C");

  gMdnsStarted = true;
  gLastQueryMs = 0;

  Serial.printf("mDNS started: %s.local\n", gHostname.c_str());
  Serial.printf("Advertising _osc._tcp on port %u\n", gOscTcpPort);
}

void discoverQlab() {
  if (!gMdnsStarted) {
    return;
  }

  unsigned long now = millis();
  if (gLastQueryMs != 0 && now - gLastQueryMs < kQlabQueryIntervalMs) {
    return;
  }
  gLastQueryMs = now;

  int serviceCount = MDNS.queryService("qlab", "udp");
  if (serviceCount <= 0) {
    if (gQlabCount != 0) {
      Serial.println("QLab mDNS services disappeared");
    }
    clearQlabTargets();
    Serial.println("Browsing for _qlab._udp: no services found");
    return;
  }

  IPAddress nextIps[kMaxQlabTargets];
  uint16_t nextPorts[kMaxQlabTargets] = {0};
  size_t nextCount = 0;

  for (int i = 0; i < serviceCount; ++i) {
    IPAddress candidateIp = MDNS.IP(i);
    uint16_t candidatePort = MDNS.port(i);
    if (!candidateIp || candidatePort == 0 || nextCount >= kMaxQlabTargets) {
      continue;
    }

    bool duplicate = false;
    for (size_t j = 0; j < nextCount; ++j) {
      if (nextIps[j] == candidateIp && nextPorts[j] == candidatePort) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      continue;
    }

    nextIps[nextCount] = candidateIp;
    nextPorts[nextCount] = candidatePort;
    ++nextCount;
  }

  bool changed = nextCount != gQlabCount;
  if (!changed) {
    for (size_t i = 0; i < nextCount; ++i) {
      if (nextIps[i] != gQlabIps[i] || nextPorts[i] != gQlabPorts[i]) {
        changed = true;
        break;
      }
    }
  }

  if (nextCount == 0) {
    bool hadCachedTargets = gQlabCount != 0;
    bool cacheExpired = hadCachedTargets && gLastResolvedQlabMs != 0 &&
                        now - gLastResolvedQlabMs >= kQlabStaleTimeoutMs;
    if (cacheExpired) {
      clearQlabTargets();
      Serial.println("QLab mDNS results had no IPv4 address, clearing stale cached targets");
    } else {
      Serial.println("QLab mDNS results had no IPv4 address, keeping last resolved target(s)");
    }
    return;
  }

  for (size_t i = 0; i < kMaxQlabTargets; ++i) {
    gQlabIps[i] = IPAddress();
    gQlabPorts[i] = 0;
  }
  for (size_t i = 0; i < nextCount; ++i) {
    gQlabIps[i] = nextIps[i];
    gQlabPorts[i] = nextPorts[i];
  }
  gQlabCount = nextCount;
  gLastResolvedQlabMs = now;

  if (changed && gQlabCount != 0) {
    Serial.printf("QLab targets discovered: %u\n", static_cast<unsigned>(gQlabCount));
    for (size_t i = 0; i < gQlabCount; ++i) {
      Serial.printf("  QLab %u at %s:%u\n",
                    static_cast<unsigned>(i + 1),
                    gQlabIps[i].toString().c_str(),
                    gQlabPorts[i]);
    }
  }

}

}  // namespace

void MDNS_Configure(const char *hostname, uint16_t oscTcpPort) {
  if (hostname && hostname[0] != '\0') {
    gHostname = hostname;
  }
  if (oscTcpPort != 0) {
    gOscTcpPort = oscTcpPort;
  }
}

void MDNS_Loop(void) {
  if (!ETH_Connected()) {
    stopMdns();
    return;
  }

  startMdns();
  discoverQlab();
}

bool MDNS_QlabAvailable(void) {
  return gQlabCount != 0;
}

size_t MDNS_QlabCount(void) {
  return gQlabCount;
}

IPAddress MDNS_QlabIP(size_t index) {
  if (index >= gQlabCount) {
    return IPAddress();
  }
  return gQlabIps[index];
}

uint16_t MDNS_QlabPort(size_t index) {
  if (index >= gQlabCount) {
    return 0;
  }
  return gQlabPorts[index];
}
