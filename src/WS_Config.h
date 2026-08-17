#pragma once

#include <Arduino.h>

constexpr uint8_t CONFIG_VERSION = 4;
constexpr uint8_t DIN_COUNT = 8;
constexpr uint8_t RELAY_COUNT = 8;
constexpr uint8_t DIN_EVENT_COUNT = 2;
constexpr uint8_t MAX_DIN_MESSAGES = 1;
constexpr uint8_t MAX_RELAY_RULES = 3;
constexpr uint8_t OSC_ARG_COUNT = 4;
constexpr size_t OSC_ADDRESS_LENGTH = 64;
constexpr size_t OSC_STRING_LENGTH = 160;
constexpr size_t TARGET_LENGTH = 16;
constexpr size_t OSC_MESSAGE_LENGTH = 193;

enum ControlSource : uint8_t {
  SOURCE_OSC = 0,
  SOURCE_ARTNET = 1,
  SOURCE_SACN = 2,
};

enum OscArgType : uint8_t {
  ARG_NONE = 0,
  ARG_INT32 = 1,
  ARG_FLOAT = 2,
  ARG_STRING = 3,
  ARG_BOOL = 4,
};

enum TargetType : uint8_t {
  TARGET_IP = 0,
  TARGET_QLAB = 1,
};

enum OutputTransport : uint8_t {
  OUTPUT_UDP = 0,
  OUTPUT_TCP = 1,
};

enum MatchMode : uint8_t {
  MATCH_ANY_ARGS = 0,
  MATCH_NO_ARGS = 1,
  MATCH_EXACT_ARGS = 2,
};

enum RelayAction : uint8_t {
  RELAY_ACTION_OFF = 0,
  RELAY_ACTION_ON = 1,
  RELAY_ACTION_PULSE = 2,
};

constexpr RelayAction RelayRuleAction(uint8_t ruleIndex) {
  return ruleIndex == 0 ? RELAY_ACTION_OFF : ruleIndex == 1 ? RELAY_ACTION_ON : RELAY_ACTION_PULSE;
}

struct OscArgumentConfig {
  uint8_t type;
  int32_t intValue;
  float floatValue;
  bool boolValue;
  char stringValue[OSC_STRING_LENGTH];
};

struct DinMessageConfig {
  bool enabled;
  uint8_t targetType;
  uint8_t transport;
  uint16_t port;
  bool repeat;
  uint32_t repeatIntervalMs;
  char target[TARGET_LENGTH];
  char message[OSC_MESSAGE_LENGTH];
};

struct RelayOscRuleConfig {
  bool enabled;
  uint8_t matchMode;
  uint32_t pulseDurationMs;
  char matchMessage[OSC_MESSAGE_LENGTH];
};

struct RelayDmxConfig {
  uint16_t universe;
  uint16_t channel;
  uint8_t onLevel;
  uint8_t offLevel;
};

struct DeviceConfig {
  uint32_t magic;
  uint8_t version;
  uint16_t oscPort;
  uint16_t debounceMs;
  uint32_t dmxTimeoutMs;
  bool qlabDiscovery;
  uint8_t relaySource[RELAY_COUNT];
  RelayDmxConfig artnet[RELAY_COUNT];
  RelayDmxConfig sacn[RELAY_COUNT];
  DinMessageConfig din[DIN_COUNT][DIN_EVENT_COUNT][MAX_DIN_MESSAGES];
  RelayOscRuleConfig relayOsc[RELAY_COUNT][MAX_RELAY_RULES];
};

void Config_Defaults(DeviceConfig &config);
bool Config_Load(DeviceConfig &config);
bool Config_Save(const DeviceConfig &config);
bool Config_IsValid(const DeviceConfig &config, String &error);
