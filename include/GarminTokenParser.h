#pragma once

#include <ArduinoJson.h>

#include <stddef.h>

namespace pixelos {

// Parse only the two fields PikOS retains from Garmin's DI token response.
// `payload` must be mutable: ArduinoJson then decodes strings in place instead
// of duplicating multi-kilobyte JWTs into its memory pool. The returned
// pointers remain valid until the caller changes or destroys that buffer.
inline bool parseGarminTokenJson(char* payload, size_t length,
                                 const char*& accessToken,
                                 const char*& refreshToken,
                                 const char*& parseError) {
  accessToken = nullptr;
  refreshToken = nullptr;
  parseError = nullptr;
  if (payload == nullptr || length == 0) {
    parseError = "EmptyInput";
    return false;
  }

  StaticJsonDocument<128> filter;
  filter["access_token"] = true;
  filter["refresh_token"] = true;
  if (filter.overflowed()) {
    parseError = "FilterOverflow";
    return false;
  }

  // In zero-copy mode the document owns only an object and two variant slots;
  // both token strings continue to live inside `payload`.
  StaticJsonDocument<256> document;
  const DeserializationError error =
      deserializeJson(document, payload, length,
                      DeserializationOption::Filter(filter));
  if (error) {
    // ArduinoJson returns pointers to static error-name literals.
    parseError = error.c_str();
    return false;
  }
  accessToken = document["access_token"].as<const char*>();
  refreshToken = document["refresh_token"].as<const char*>();
  return true;
}

}  // namespace pixelos
