#include "WS_MDNS.h"

#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "WS_ETH.h"

namespace {

constexpr uint32_t kQlabQueryIntervalMs = 5000;
constexpr uint32_t kMdnsTaskIntervalMs = 250;
constexpr size_t kMaxQlabTargets = 8;
constexpr uint32_t kQlabStaleTimeoutMs = 30000;
constexpr size_t kIpStringLength = 16;

String gHostname = "esp32-eth0";
uint16_t gOscTcpPort = 53000;
bool gMdnsStarted = false;
unsigned long gLastQueryMs = 0;
IPAddress gQlabIps[kMaxQlabTargets];
char gQlabIpStrings[kMaxQlabTargets][kIpStringLength] = {};
uint16_t gQlabPorts[kMaxQlabTargets] = {0};
size_t gQlabCount = 0;
unsigned long gLastResolvedQlabMs = 0;
TaskHandle_t gMdnsTaskHandle = nullptr;
portMUX_TYPE gQlabTargetsMux = portMUX_INITIALIZER_UNLOCKED;

void clearQlabTargets() {
  portENTER_CRITICAL(&gQlabTargetsMux);
  gQlabCount = 0;
  gLastResolvedQlabMs = 0;
  for (size_t i = 0; i < kMaxQlabTargets; ++i) {
    gQlabIps[i] = IPAddress();
    gQlabIpStrings[i][0] = '\0';
    gQlabPorts[i] = 0;
  }
  portEXIT_CRITICAL(&gQlabTargetsMux);
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
  char nextIpStrings[kMaxQlabTargets][kIpStringLength] = {};
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
    snprintf(nextIpStrings[nextCount],
             sizeof(nextIpStrings[nextCount]),
             "%u.%u.%u.%u",
             candidateIp[0],
             candidateIp[1],
             candidateIp[2],
             candidateIp[3]);
    nextPorts[nextCount] = candidatePort;
    ++nextCount;
  }

  portENTER_CRITICAL(&gQlabTargetsMux);
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
      portEXIT_CRITICAL(&gQlabTargetsMux);
      clearQlabTargets();
      Serial.println("QLab mDNS results had no IPv4 address, clearing stale cached targets");
    } else {
      portEXIT_CRITICAL(&gQlabTargetsMux);
      Serial.println("QLab mDNS results had no IPv4 address, keeping last resolved target(s)");
    }
    return;
  }

  for (size_t i = 0; i < kMaxQlabTargets; ++i) {
    gQlabIps[i] = IPAddress();
    gQlabIpStrings[i][0] = '\0';
    gQlabPorts[i] = 0;
  }
  for (size_t i = 0; i < nextCount; ++i) {
    gQlabIps[i] = nextIps[i];
    strlcpy(gQlabIpStrings[i], nextIpStrings[i], sizeof(gQlabIpStrings[i]));
    gQlabPorts[i] = nextPorts[i];
  }
  gQlabCount = nextCount;
  gLastResolvedQlabMs = now;
  portEXIT_CRITICAL(&gQlabTargetsMux);

  if (changed && nextCount != 0) {
    Serial.printf("QLab targets discovered: %u\n", static_cast<unsigned>(nextCount));
    for (size_t i = 0; i < nextCount; ++i) {
      Serial.printf("  QLab %u at %s:%u\n",
                    static_cast<unsigned>(i + 1),
                    nextIpStrings[i],
                    nextPorts[i]);
    }
  }

}

void mdnsTask(void *parameter) {
  (void)parameter;

  while (true) {
    if (!ETH_Connected()) {
      stopMdns();
    } else {
      startMdns();
      discoverQlab();
    }

    vTaskDelay(pdMS_TO_TICKS(kMdnsTaskIntervalMs));
  }
}

void ensureMdnsTask() {
  if (gMdnsTaskHandle != nullptr) {
    return;
  }

  xTaskCreatePinnedToCore(
    mdnsTask,
    "MDNSTask",
    4096,
    nullptr,
    1,
    &gMdnsTaskHandle,
    0
  );
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
  ensureMdnsTask();
}

bool MDNS_QlabAvailable(void) {
  portENTER_CRITICAL(&gQlabTargetsMux);
  bool available = gQlabCount != 0;
  portEXIT_CRITICAL(&gQlabTargetsMux);
  return available;
}

size_t MDNS_QlabCount(void) {
  portENTER_CRITICAL(&gQlabTargetsMux);
  size_t count = gQlabCount;
  portEXIT_CRITICAL(&gQlabTargetsMux);
  return count;
}

IPAddress MDNS_QlabIP(size_t index) {
  portENTER_CRITICAL(&gQlabTargetsMux);
  if (index >= gQlabCount) {
    portEXIT_CRITICAL(&gQlabTargetsMux);
    return IPAddress();
  }
  IPAddress ip = gQlabIps[index];
  portEXIT_CRITICAL(&gQlabTargetsMux);
  return ip;
}

uint16_t MDNS_QlabPort(size_t index) {
  portENTER_CRITICAL(&gQlabTargetsMux);
  if (index >= gQlabCount) {
    portEXIT_CRITICAL(&gQlabTargetsMux);
    return 0;
  }
  uint16_t port = gQlabPorts[index];
  portEXIT_CRITICAL(&gQlabTargetsMux);
  return port;
}

bool MDNS_QlabTarget(size_t index, char *ipString, size_t ipStringSize, uint16_t *port) {
  if (ipString == nullptr || ipStringSize == 0 || port == nullptr) {
    return false;
  }

  portENTER_CRITICAL(&gQlabTargetsMux);
  if (index >= gQlabCount || gQlabIpStrings[index][0] == '\0' || gQlabPorts[index] == 0) {
    portEXIT_CRITICAL(&gQlabTargetsMux);
    ipString[0] = '\0';
    *port = 0;
    return false;
  }

  strlcpy(ipString, gQlabIpStrings[index], ipStringSize);
  *port = gQlabPorts[index];
  portEXIT_CRITICAL(&gQlabTargetsMux);
  return true;
}
