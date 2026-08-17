#include "WS_OscText.h"

#include <ctype.h>
#include <stdlib.h>

namespace {
void clearArg(OscArgumentConfig &arg) {
  memset(&arg, 0, sizeof(arg));
  arg.type = ARG_NONE;
}

void skipSpaces(const char *&cursor) {
  while (*cursor && isspace(static_cast<unsigned char>(*cursor))) ++cursor;
}

bool readToken(const char *&cursor, char *output, size_t outputSize, bool &quoted, String &error) {
  skipSpaces(cursor);
  if (!*cursor) return false;
  size_t length = 0;
  quoted = *cursor == '"';
  if (quoted) ++cursor;

  while (*cursor) {
    char ch = *cursor++;
    if (quoted && ch == '"') {
      if (length >= outputSize) { error = "OSC string argument is too long"; return false; }
      output[length] = '\0';
      if (*cursor && !isspace(static_cast<unsigned char>(*cursor))) { error = "Characters must be separated by spaces"; return false; }
      return true;
    }
    if (!quoted && isspace(static_cast<unsigned char>(ch))) {
      if (length >= outputSize) { error = "OSC argument is too long"; return false; }
      output[length] = '\0';
      return true;
    }
    if (ch == '\\' && quoted && *cursor) {
      char escaped = *cursor++;
      if (escaped != '"' && escaped != '\\') { error = "Unsupported string escape"; return false; }
      ch = escaped;
    }
    if (length + 1 >= outputSize) { error = "OSC argument is too long"; return false; }
    output[length++] = ch;
  }

  if (quoted) error = "Unterminated quoted OSC string";
  else {
    if (length >= outputSize) { error = "OSC argument is too long"; return false; }
    output[length] = '\0';
  }
  return !quoted;
}

bool isInteger(const char *text, int32_t &value) {
  if (!text[0]) return false;
  char *end = nullptr;
  long parsed = strtol(text, &end, 10);
  if (*end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX) return false;
  value = static_cast<int32_t>(parsed);
  return true;
}

bool isFloat(const char *text, float &value) {
  if (!text[0]) return false;
  bool decimal = false;
  for (const char *p = text; *p; ++p) if (*p == '.' || *p == 'e' || *p == 'E') decimal = true;
  if (!decimal) return false;
  char *end = nullptr;
  value = strtof(text, &end);
  return *end == '\0';
}
}

bool OscText_Parse(const char *text, ParsedOscText &parsed, String &error) {
  memset(&parsed, 0, sizeof(parsed));
  for (OscArgumentConfig &arg : parsed.args) clearArg(arg);
  parsed.argumentCount = 0;
  error = "";
  if (text == nullptr || text[0] != '/') { error = "OSC message must begin with an address"; return false; }

  const char *cursor = text;
  char token[OSC_STRING_LENGTH];
  bool quoted = false;
  if (!readToken(cursor, token, sizeof(token), quoted, error)) { if (error.length() == 0) error = "OSC address is missing"; return false; }
  if (quoted || token[0] != '/' || strlen(token) >= sizeof(parsed.address)) { error = "Invalid OSC address"; return false; }
  strlcpy(parsed.address, token, sizeof(parsed.address));

  while (true) {
    skipSpaces(cursor);
    if (!*cursor) break;
    if (parsed.argumentCount >= OSC_ARG_COUNT) { error = "At most four OSC arguments are supported"; return false; }
    if (!readToken(cursor, token, sizeof(token), quoted, error)) return false;
    OscArgumentConfig &arg = parsed.args[parsed.argumentCount];
    clearArg(arg);
    if (!quoted && strcmp(token, "\\T") == 0) { arg.type = ARG_BOOL; arg.boolValue = true; }
    else if (!quoted && strcmp(token, "\\F") == 0) { arg.type = ARG_BOOL; arg.boolValue = false; }
    else if (!quoted && isInteger(token, arg.intValue)) arg.type = ARG_INT32;
    else if (!quoted && isFloat(token, arg.floatValue)) arg.type = ARG_FLOAT;
    else if (quoted) { arg.type = ARG_STRING; strlcpy(arg.stringValue, token, sizeof(arg.stringValue)); }
    else { error = "Unsupported OSC argument; use integer, float, boolean, or quoted string"; return false; }
    ++parsed.argumentCount;
  }
  return true;
}

