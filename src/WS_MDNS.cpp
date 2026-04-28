#include "WS_MDNS.h"

#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "WS_ETH.h"

namespace {

constexpr uint32_t kQlabQueryIntervalMs = 5000;
constexpr uint32_t kMdnsTaskIntervalMs = 250;
constexpr uint32_t kQlabHostResolveTimeoutMs = 1200;
constexpr size_t kMaxQlabTargets = 8;
constexpr size_t kIpStringLength = 16;

String gHostname = "esp32-eth0";
uint16_t gOscTcpPort = 53000;
bool gMdnsStarted = false;
unsigned long gLastQueryMs = 0;
IPAddress gQlabIps[kMaxQlabTargets];
char gQlabIpStrings[kMaxQlabTargets][kIpStringLength] = {};
uint16_t gQlabPorts[kMaxQlabTargets] = {0};
size_t gQlabCount = 0;
TaskHandle_t gMdnsTaskHandle = nullptr;
portMUX_TYPE gQlabTargetsMux = portMUX_INITIALIZER_UNLOCKED;

void stopMdns() {
  if (!gMdnsStarted) {
    return;
  }

  MDNS.end();
  gMdnsStarted = false;
  gLastQueryMs = 0;
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

IPAddress resolveQlabServiceIp(int serviceIndex) {
  IPAddress ip = MDNS.IP(serviceIndex);
  if (ip) {
    return ip;
  }

  String hostName = MDNS.hostname(serviceIndex);
  if (hostName.length() == 0) {
    return IPAddress();
  }
  if (hostName.endsWith(".local")) {
    hostName.remove(hostName.length() - 6);
  }

  return MDNS.queryHost(hostName, kQlabHostResolveTimeoutMs);
}

bool addQlabTarget(IPAddress ip, uint16_t port, char *ipString, size_t ipStringSize) {
  if (!ip || port == 0 || ipString == nullptr || ipStringSize == 0) {
    return false;
  }

  char candidateIpString[kIpStringLength];
  snprintf(candidateIpString,
           sizeof(candidateIpString),
           "%u.%u.%u.%u",
           ip[0],
           ip[1],
           ip[2],
           ip[3]);

  portENTER_CRITICAL(&gQlabTargetsMux);
  for (size_t i = 0; i < gQlabCount; ++i) {
    if (gQlabIps[i] == ip && gQlabPorts[i] == port) {
      portEXIT_CRITICAL(&gQlabTargetsMux);
      return false;
    }
  }

  if (gQlabCount >= kMaxQlabTargets) {
    portEXIT_CRITICAL(&gQlabTargetsMux);
    return false;
  }

  size_t index = gQlabCount;
  gQlabIps[index] = ip;
  strlcpy(gQlabIpStrings[index], candidateIpString, sizeof(gQlabIpStrings[index]));
  gQlabPorts[index] = port;
  ++gQlabCount;
  portEXIT_CRITICAL(&gQlabTargetsMux);

  strlcpy(ipString, candidateIpString, ipStringSize);
  return true;
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
    if (MDNS_QlabAvailable()) {
      Serial.println("QLab mDNS services disappeared, keeping cached target(s)");
    } else {
      Serial.println("Browsing for _qlab._udp: no services found");
    }
    return;
  }

  size_t addedCount = 0;

  for (int i = 0; i < serviceCount; ++i) {
    IPAddress candidateIp = resolveQlabServiceIp(i);
    uint16_t candidatePort = MDNS.port(i);
    char candidateIpString[kIpStringLength] = {};
    if (!addQlabTarget(candidateIp, candidatePort, candidateIpString, sizeof(candidateIpString))) {
      continue;
    }

    ++addedCount;
    Serial.printf("QLab target added: %s:%u\n", candidateIpString, candidatePort);
  }

  if (addedCount == 0 && !MDNS_QlabAvailable()) {
    Serial.println("QLab mDNS results had no IPv4 address");
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
