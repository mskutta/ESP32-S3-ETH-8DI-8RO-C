#include "WS_Config.h"
#include "WS_OscText.h"

#include <Preferences.h>

namespace {
constexpr uint32_t kConfigMagic = 0x524C5932; // RLY2
constexpr char kNamespace[] = "relay-config";
constexpr char kPartition[] = "config";
// NVS needs space for the old blob, its replacement, and garbage collection.
static_assert(2 * sizeof(DeviceConfig) + 8192 < 65536, "DeviceConfig exceeds the configuration NVS budget");

void setAddress(char *destination, size_t size, const char *value) {
  if (value == nullptr) value = "";
  strlcpy(destination, value, size);
}

bool readConfig(DeviceConfig &config, const char *partition) {
  Preferences preferences;
  if (!preferences.begin(kNamespace, true, partition)) return false;
  bool valid = preferences.getBytesLength("blob") == sizeof(config) &&
               preferences.getBytes("blob", &config, sizeof(config)) == sizeof(config);
  preferences.end();
  return valid && config.magic == kConfigMagic && config.version == CONFIG_VERSION;
}
}

void Config_Defaults(DeviceConfig &config) {
  memset(&config, 0, sizeof(config));
  config.magic = kConfigMagic;
  config.version = CONFIG_VERSION;
  config.oscPort = 53000;
  config.debounceMs = 50;
  config.dmxTimeoutMs = 2000;
  config.qlabDiscovery = true;

  for (uint8_t relay = 0; relay < RELAY_COUNT; ++relay) {
    config.relaySource[relay] = SOURCE_OSC;
    config.artnet[relay] = {1, static_cast<uint16_t>(relay + 1), 128, 100};
    config.sacn[relay] = {1, static_cast<uint16_t>(relay + 1), 128, 100};

    RelayOscRuleConfig &off = config.relayOsc[relay][0];
    off.enabled = true;
    off.matchMode = MATCH_EXACT_ARGS;
    setAddress(off.matchMessage, sizeof(off.matchMessage), (String("/relay/") + String(relay + 1) + " 0").c_str());

    RelayOscRuleConfig &on = config.relayOsc[relay][1];
    on.enabled = true;
    on.matchMode = MATCH_EXACT_ARGS;
    setAddress(on.matchMessage, sizeof(on.matchMessage), (String("/relay/") + String(relay + 1) + " 1").c_str());

    RelayOscRuleConfig &pulse = config.relayOsc[relay][2];
    pulse.enabled = true;
    pulse.matchMode = MATCH_EXACT_ARGS;
    pulse.pulseDurationMs = 1000;
    setAddress(pulse.matchMessage, sizeof(pulse.matchMessage), (String("/relay/") + String(relay + 1) + " 2").c_str());
  }
}

bool Config_Load(DeviceConfig &config) {
  if (readConfig(config, kPartition)) return true;
  // Keep the original NVS partition untouched so installing the new partition
  // table preserves settings and migration can be retried after a write error.
  if (readConfig(config, nullptr)) {
    Config_Save(config);
    return true;
  }
  Config_Defaults(config);
  Config_Save(config);
  return false;
}

bool Config_Save(const DeviceConfig &config) {
  Preferences preferences;
  if (!preferences.begin(kNamespace, false, kPartition)) return false;
  size_t written = preferences.putBytes("blob", &config, sizeof(config));
  preferences.end();
  return written == sizeof(config);
}

bool Config_IsValid(const DeviceConfig &config, String &error) {
  if (config.magic != kConfigMagic || config.version != CONFIG_VERSION) {
    error = "Unsupported configuration version";
    return false;
  }
  if (config.oscPort == 0 || config.debounceMs > 5000 || config.dmxTimeoutMs < 100 || config.dmxTimeoutMs > 60000) {
    error = "Invalid global timing or OSC port";
    return false;
  }

  for (uint8_t relay = 0; relay < RELAY_COUNT; ++relay) {
    if (config.relaySource[relay] > SOURCE_SACN) {
      error = "Invalid relay source";
      return false;
    }
    const RelayDmxConfig *dmx[] = {&config.artnet[relay], &config.sacn[relay]};
    for (const RelayDmxConfig *mapping : dmx) {
      if (mapping->universe == 0 || mapping->channel == 0 || mapping->channel > 512 || mapping->offLevel > mapping->onLevel) {
        error = "Invalid DMX mapping or hysteresis thresholds";
        return false;
      }
    }
    for (uint8_t ruleIndex = 0; ruleIndex < MAX_RELAY_RULES; ++ruleIndex) {
      const RelayOscRuleConfig &rule = config.relayOsc[relay][ruleIndex];
      if (!rule.enabled) continue;
      if (rule.matchMessage[0] != '/' || strlen(rule.matchMessage) >= OSC_MESSAGE_LENGTH || rule.matchMode > MATCH_EXACT_ARGS) {
        error = "Invalid relay OSC rule";
        return false;
      }
      ParsedOscText parsed;
      String parseError;
      if (!OscText_Parse(rule.matchMessage, parsed, parseError)) {
        error = "Relay rule: " + parseError;
        return false;
      }
      if (rule.matchMode == MATCH_ANY_ARGS && parsed.argumentCount != 0) {
        error = "Any-arguments relay rules must contain only an OSC address";
        return false;
      }
      if (rule.matchMode == MATCH_NO_ARGS && parsed.argumentCount != 0) {
        error = "No-arguments relay rules must contain only an OSC address";
        return false;
      }
      if (RelayRuleAction(ruleIndex) == RELAY_ACTION_PULSE && (rule.pulseDurationMs == 0 || rule.pulseDurationMs > 86400000UL)) {
        error = "Invalid relay pulse duration";
        return false;
      }
    }
  }

  for (uint8_t input = 0; input < DIN_COUNT; ++input) {
    for (uint8_t event = 0; event < DIN_EVENT_COUNT; ++event) {
      for (uint8_t index = 0; index < MAX_DIN_MESSAGES; ++index) {
        const DinMessageConfig &message = config.din[input][event][index];
        if (!message.enabled) continue;
        String context = String("Input ") + String(input + 1) + (event ? " Closed: " : " Open: ");
        if (message.message[0] != '/') {
          error = context + "OSC message must start with /";
          return false;
        }
        if (strnlen(message.message, sizeof(message.message)) >= OSC_MESSAGE_LENGTH) {
          error = context + "OSC message must be at most 192 characters";
          return false;
        }
        if (message.targetType > TARGET_QLAB) {
          error = context + "select IP address or QLab as the target";
          return false;
        }
        if (message.transport > OUTPUT_TCP) {
          error = context + "select UDP or TCP as the transport";
          return false;
        }
        if (message.targetType == TARGET_IP && message.port == 0) {
          error = context + "destination port must be 1–65535 (QLab usually uses 53000)";
          return false;
        }
        if (message.repeat && message.repeatIntervalMs < 20) {
          error = context + "repeat interval must be at least 20 ms when Repeat is enabled";
          return false;
        }
        ParsedOscText parsed;
        String parseError;
        if (!OscText_Parse(message.message, parsed, parseError)) {
          error = context + "OSC message: " + parseError;
          return false;
        }
        if (message.targetType == TARGET_IP) {
          IPAddress ip;
          if (!ip.fromString(message.target)) {
            error = context + "enter a valid IPv4 target";
            return false;
          }
        }
      }
    }
  }
  return true;
}
