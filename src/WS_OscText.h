#pragma once

#include "WS_Config.h"

struct ParsedOscText {
  char address[OSC_ADDRESS_LENGTH];
  OscArgumentConfig args[OSC_ARG_COUNT];
  uint8_t argumentCount;
};

bool OscText_Parse(const char *text, ParsedOscText &parsed, String &error);

