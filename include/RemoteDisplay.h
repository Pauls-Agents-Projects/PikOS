#pragma once

#include <Arduino.h>
#include <FastLED.h>

#include "Config.h"
#include "Hardware.h"

namespace pixelos {

// Lightweight, authenticated HTTP canvas used by two-stage apps. The former
// OpenRGB and E1.31 servers opened three extra lwIP listeners and carried a
// 16 KiB parser allocation even though those integrations were retired.
class RemoteDisplay {
 public:
  void begin(const AppConfig&) {}
  void tick(const AppConfig&, uint32_t) {}
  bool active(uint32_t nowMs, const AppConfig& config) const;
  const CRGB* frame() const { return frame_; }
  uint32_t frameSequence() const { return frameSequence_; }
  String lastProtocol() const { return frameSequence_ ? "HTTP" : ""; }
  bool setJsonFrame(JsonArrayConst pixels, const IPAddress& source,
                    const AppConfig& config);

 private:
  bool sourceAllowed(const IPAddress& source,
                     const AppConfig& config) const;

  CRGB frame_[hardware::kLedCount]{};
  uint32_t frameSequence_ = 0;
  uint32_t lastFrameMs_ = 0;
};

}  // namespace pixelos
