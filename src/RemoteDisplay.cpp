#include "RemoteDisplay.h"

namespace pixelos {

namespace {

CRGB colorFromInteger(uint32_t color) {
  return CRGB((color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}

}  // namespace

bool RemoteDisplay::active(uint32_t nowMs, const AppConfig& config) const {
  return frameSequence_ != 0 &&
         nowMs - lastFrameMs_ <= config.remoteTimeoutMs;
}

bool RemoteDisplay::sourceAllowed(const IPAddress& source,
                                  const AppConfig& config) const {
  if (config.remoteSourceIp.isEmpty()) return true;
  IPAddress allowed;
  return allowed.fromString(config.remoteSourceIp) && allowed == source;
}

bool RemoteDisplay::setJsonFrame(JsonArrayConst pixels,
                                 const IPAddress& source,
                                 const AppConfig& config) {
  if (!sourceAllowed(source, config) ||
      pixels.size() != hardware::kLedCount) {
    return false;
  }
  size_t index = 0;
  for (JsonVariantConst pixel : pixels) {
    frame_[index++] = colorFromInteger(pixel.as<uint32_t>());
  }
  ++frameSequence_;
  lastFrameMs_ = millis();
  return true;
}

}  // namespace pixelos
