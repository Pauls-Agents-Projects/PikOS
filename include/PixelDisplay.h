#pragma once

#include <Arduino.h>
#include <FastLED.h>

#include "Hardware.h"

namespace pixelos {

enum class TinyIcon : uint8_t {
  None,
  Clock,
  Hourglass,
  Thermometer,
  Weather,
  Wind,
  Rain,
  Transit,
  Rss,
  Battery,
  Calendar,
  Activity,
  Reddit
};

class PixelDisplay {
 public:
  void begin();
  void clear();
  void setPixel(int16_t x, int16_t y, const CRGB& color);
  CRGB getPixel(int16_t x, int16_t y) const;
  void drawText(int16_t x, int16_t y, const String& text, const CRGB& color);
  void drawClockText(int16_t x, int16_t y, const String& text,
                     const CRGB& color);
  void drawRainbowText(int16_t x, int16_t y, const String& text,
                       uint8_t startHue);
  void drawClockRainbowText(int16_t x, int16_t y, const String& text,
                            uint8_t startHue);
  void drawIcon(TinyIcon icon, int16_t x, int16_t y,
                const CRGB& primary, uint32_t phaseMs = 0,
                uint8_t level = 50);
  void drawAnalogClock(int16_t x, int16_t y, const CRGB& primary,
                       uint8_t hour, uint8_t minute);
  void drawCentered(const String& text, const CRGB& color, int16_t y = 0);
  void show();
  void showFrame(const CRGB* logicalFrame);
  void setBrightness(uint8_t value);
  void setOrientation(bool orientation180);
  uint8_t brightness() const { return brightness_; }
  uint32_t frameSequence() const { return frameSequence_; }
  uint32_t transmittedFrames() const { return transmittedFrames_; }
  uint32_t skippedFrames() const { return skippedFrames_; }
  uint32_t maximumShowUs() const { return maximumShowUs_; }

 private:
  uint16_t physicalIndex(int16_t x, int16_t y) const;

  CRGB leds_[hardware::kLedCount]{};
  uint8_t brightness_ = 32;
  bool orientation180_ = false;
  uint32_t frameSequence_ = 0;
  uint32_t lastFrameHash_ = 0;
  uint32_t transmittedFrames_ = 0;
  uint32_t skippedFrames_ = 0;
  uint32_t maximumShowUs_ = 0;
  bool forceShow_ = true;
};

}  // namespace pixelos
