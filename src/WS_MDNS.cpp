#include "WS_MDNS.h"

#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "WS_ETH.h"

namespace {

constexpr uint32_t kQlabQueryIntervalMs = 5000;
constexpr uint32_t kMdnsTaskIntervalMs = 250;
constexpr uint32_t kQlabHostResolveTimeoutMs = 1200;
constexpr uint32_t kQlabTargetStaleTimeoutMs = 30000;
constexpr size_t kMaxQlabTargets = 8;
constexpr size_t kIpStringLength = 16;
constexpr size_t kHostnameLength = 64;

String gHostname = "esp32-eth0";
String gInstanceName = "SpookIO";
uint16_t gOscTcpPort = 53000;
bool gMdnsStarted = false;
bool gQlabDiscoveryEnabled = true;
unsigned long gLastQueryMs = 0;
IPAddress gQlabIps[kMaxQlabTargets];
char gQlabIpStrings[kMaxQlabTargets][kIpStringLength] = {};
char gQlabHostnames[kMaxQlabTargets][kHostnameLength] = {};
uint16_t gQlabPorts[kMaxQlabTargets] = {0};
unsigned long gQlabLastSeenMs[kMaxQlabTargets] = {0};
size_t gQlabCount = 0;
TaskHandle_t gMdnsTaskHandle = nullptr;
portMUX_TYPE gQlabTargetsMux = portMUX_INITIALIZER_UNLOCKED;

void clearQlabTargets();

void stopMdns() {
  if (gMdnsStarted) {
    MDNS.end();
    gMdnsStarted = false;
    Serial.println("mDNS stopped");
  }

  gLastQueryMs = 0;
  clearQlabTargets();
}

void startMdns() {
  if (gMdnsStarted || !ETH_Connected()) {
    return;
  }

  if (!MDNS.begin(gHostname.c_str())) {
    Serial.printf("mDNS start failed for %s.local\n", gHostname.c_str());
    return;
  }

  MDNS.setInstanceName(gInstanceName);
  MDNS.addService("osc", "tcp", gOscTcpPort);
  MDNS.addServiceTxt("osc", "tcp", "transport", "tcp");
  MDNS.addServiceTxt("osc", "tcp", "path", "/relay/*");
  MDNS.addServiceTxt("osc", "tcp", "board", "ESP32-S3-ETH-8DI-8RO-C");

  gMdnsStarted = true;
  gLastQueryMs = 0;

  Serial.printf("mDNS started: %s.local\n", gHostname.c_str());
  Serial.printf("Advertising _osc._tcp on port %u\n", gOscTcpPort);
}

IPAddress resolveQlabServiceIp(int serviceIndex, String &hostName) {
  hostName = MDNS.hostname(serviceIndex);
  if (hostName.endsWith(".local")) {
    hostName.remove(hostName.length() - 6);
  }

  IPAddress ip = MDNS.IP(serviceIndex);
  if (ip) {
    return ip;
  }

  if (hostName.length() == 0) {
    return IPAddress();
  }
  if (hostName.endsWith(".local")) {
    hostName.remove(hostName.length() - 6);
  }

  return MDNS.queryHost(hostName, kQlabHostResolveTimeoutMs);
}

bool updateQlabTarget(const char *hostname, IPAddress ip, uint16_t port, char *ipString, size_t ipStringSize, bool *changedOut) {
  if (hostname == nullptr || hostname[0] == '\0' || !ip || port == 0 || ipString == nullptr || ipStringSize == 0) {
    return false;
  }
  if (changedOut != nullptr) *changedOut = false;

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
    if (strcmp(gQlabHostnames[i], hostname) == 0) {
      bool changed = gQlabIps[i] != ip || gQlabPorts[i] != port;
      if (changedOut != nullptr) *changedOut = changed;
      gQlabIps[i] = ip;
      strlcpy(gQlabIpStrings[i], candidateIpString, sizeof(gQlabIpStrings[i]));
      gQlabPorts[i] = port;
      gQlabLastSeenMs[i] = millis();
      portEXIT_CRITICAL(&gQlabTargetsMux);
      strlcpy(ipString, candidateIpString, ipStringSize);
      return true;
    }
  }

  if (gQlabCount >= kMaxQlabTargets) {
    portEXIT_CRITICAL(&gQlabTargetsMux);
    return false;
  }

  size_t index = gQlabCount;
  if (changedOut != nullptr) *changedOut = true;
  gQlabIps[index] = ip;
  strlcpy(gQlabIpStrings[index], candidateIpString, sizeof(gQlabIpStrings[index]));
  strlcpy(gQlabHostnames[index], hostname, sizeof(gQlabHostnames[index]));
  gQlabPorts[index] = port;
  gQlabLastSeenMs[index] = millis();
  ++gQlabCount;
  portEXIT_CRITICAL(&gQlabTargetsMux);

  strlcpy(ipString, candidateIpString, ipStringSize);
  return true;
}

void clearQlabTargets() {
  portENTER_CRITICAL(&gQlabTargetsMux);
  gQlabCount = 0;
  memset(gQlabIps, 0, sizeof(gQlabIps));
  memset(gQlabIpStrings, 0, sizeof(gQlabIpStrings));
  memset(gQlabHostnames, 0, sizeof(gQlabHostnames));
  memset(gQlabPorts, 0, sizeof(gQlabPorts));
  memset(gQlabLastSeenMs, 0, sizeof(gQlabLastSeenMs));
  portEXIT_CRITICAL(&gQlabTargetsMux);
}

void expireQlabTargets(unsigned long now) {
  portENTER_CRITICAL(&gQlabTargetsMux);
  for (size_t i = 0; i < gQlabCount;) {
    if (gQlabLastSeenMs[i] != 0 && now - gQlabLastSeenMs[i] >= kQlabTargetStaleTimeoutMs) {
      Serial.printf("QLab target expired: %s at %s:%u\n",
                    gQlabHostnames[i], gQlabIpStrings[i], gQlabPorts[i]);
      for (size_t j = i + 1; j < gQlabCount; ++j) {
        gQlabIps[j - 1] = gQlabIps[j];
        strlcpy(gQlabIpStrings[j - 1], gQlabIpStrings[j], sizeof(gQlabIpStrings[j - 1]));
        strlcpy(gQlabHostnames[j - 1], gQlabHostnames[j], sizeof(gQlabHostnames[j - 1]));
        gQlabPorts[j - 1] = gQlabPorts[j];
        gQlabLastSeenMs[j - 1] = gQlabLastSeenMs[j];
      }
      --gQlabCount;
      gQlabIps[gQlabCount] = IPAddress();
      gQlabIpStrings[gQlabCount][0] = '\0';
      gQlabHostnames[gQlabCount][0] = '\0';
      gQlabPorts[gQlabCount] = 0;
      gQlabLastSeenMs[gQlabCount] = 0;
      continue;
    }
    ++i;
  }
  portEXIT_CRITICAL(&gQlabTargetsMux);
}

void discoverQlab() {
  if (!gMdnsStarted || !gQlabDiscoveryEnabled) {
    return;
  }

  unsigned long now = millis();
  if (gLastQueryMs != 0 && now - gLastQueryMs < kQlabQueryIntervalMs) {
    return;
  }
  gLastQueryMs = now;
  expireQlabTargets(now);

  int serviceCount = MDNS.queryService("qlab", "udp");
  if (serviceCount <= 0) {
    Serial.println("Browsing for _qlab._udp: no services found");
    return;
  }

  size_t addedCount = 0;

  for (int i = 0; i < serviceCount; ++i) {
    String hostName;
    IPAddress candidateIp = resolveQlabServiceIp(i, hostName);
    uint16_t candidatePort = MDNS.port(i);
    char candidateIpString[kIpStringLength] = {};
    bool changed = false;
    if (!updateQlabTarget(hostName.c_str(), candidateIp, candidatePort, candidateIpString, sizeof(candidateIpString), &changed)) {
      continue;
    }

    ++addedCount;
    if (changed) {
      Serial.printf("QLab target discovered: %s at %s:%u\n", hostName.c_str(), candidateIpString, candidatePort);
    }
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

void MDNS_Configure(const char *hostname, uint16_t oscTcpPort, const char *instanceName) {
  if (hostname && hostname[0] != '\0') {
    gHostname = hostname;
  }
  if (instanceName && instanceName[0] != '\0') {
    gInstanceName = instanceName;
  }
  if (oscTcpPort != 0) {
    gOscTcpPort = oscTcpPort;
  }
}

void MDNS_SetQlabDiscoveryEnabled(bool enabled) {
  gQlabDiscoveryEnabled = enabled;
  if (!enabled) {
    clearQlabTargets();
  }
}

void MDNS_Loop(void) {
  ensureMdnsTask();
}

bool MDNS_QlabAvailable(void) {
  expireQlabTargets(millis());
  portENTER_CRITICAL(&gQlabTargetsMux);
  bool available = gQlabCount != 0;
  portEXIT_CRITICAL(&gQlabTargetsMux);
  return available;
}

size_t MDNS_QlabCount(void) {
  expireQlabTargets(millis());
  portENTER_CRITICAL(&gQlabTargetsMux);
  size_t count = gQlabCount;
  portEXIT_CRITICAL(&gQlabTargetsMux);
  return count;
}

IPAddress MDNS_QlabIP(size_t index) {
  expireQlabTargets(millis());
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
  expireQlabTargets(millis());
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

  expireQlabTargets(millis());
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

bool MDNS_QlabHostname(size_t index, char *hostname, size_t hostnameSize) {
  if (hostname == nullptr || hostnameSize == 0) {
    return false;
  }

  expireQlabTargets(millis());
  portENTER_CRITICAL(&gQlabTargetsMux);
  if (index >= gQlabCount || gQlabHostnames[index][0] == '\0') {
    portEXIT_CRITICAL(&gQlabTargetsMux);
    hostname[0] = '\0';
    return false;
  }

  strlcpy(hostname, gQlabHostnames[index], hostnameSize);
  portEXIT_CRITICAL(&gQlabTargetsMux);
  return true;
}

bool MDNS_QlabTargetIsCurrent(const char *hostname, const char *ipString, uint16_t port) {
  if (hostname == nullptr || hostname[0] == '\0' || ipString == nullptr || ipString[0] == '\0' || port == 0) {
    return false;
  }

  expireQlabTargets(millis());
  portENTER_CRITICAL(&gQlabTargetsMux);
  for (size_t i = 0; i < gQlabCount; ++i) {
    if (strcmp(gQlabHostnames[i], hostname) == 0 &&
        strcmp(gQlabIpStrings[i], ipString) == 0 &&
        gQlabPorts[i] == port) {
      portEXIT_CRITICAL(&gQlabTargetsMux);
      return true;
    }
  }
  portEXIT_CRITICAL(&gQlabTargetsMux);
  return false;
}
