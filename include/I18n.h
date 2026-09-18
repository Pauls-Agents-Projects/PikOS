#pragma once

#include <Arduino.h>

namespace pixelos {

enum class TextKey : uint8_t {
  Inside,
  Outside,
  News,
  Battery,
  NoData,
  Setup,
  Address,
  Sleeping,
  Saved,
  Wifi,
  Remote,
};

const char* tr(TextKey key, const String& language);

}  // namespace pixelos
