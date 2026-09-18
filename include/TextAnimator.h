#pragma once

#include <Arduino.h>
#include <FastLED.h>

#include <vector>

#include "PixelDisplay.h"
#include "PixelFont.h"

namespace pixelos {

class LaMetricIcons;

enum class AnimationStyle : uint8_t {
  Static,
  Ticker,
  Flip,
  Slide,
  Rainbow,
  Rain,
  Magic
};

AnimationStyle parseAnimationStyle(const String& value);
const char* animationStyleName(AnimationStyle style);

struct BrowserTickerScene {
  String text;
  String previousText;
  CRGB color = CRGB::White;
  AnimationStyle style = AnimationStyle::Static;
  uint32_t key = 0;
  uint32_t elapsedMs = 0;
  uint16_t stepMs = 42;
  uint32_t holdMs = 10000;
  uint32_t baseDurationMs = 10000;
  uint32_t durationMs = 10000;
  uint16_t pageCount = 1;
  int16_t textWidth = 0;
  uint32_t customIconId = 0;
  TinyIcon icon = TinyIcon::None;
  uint8_t iconLevel = 50;
  uint8_t analogHour = UINT8_MAX;
  uint8_t analogMinute = UINT8_MAX;
  String iconLabel;
  bool clockFont = false;
  bool linkedIcon = false;
  float sparkline[23]{};
  uint8_t sparklineCount = 0;
  int8_t sparklineMarker = -1;
  bool sparklineBlinkMarker = false;
  bool sparklineBandColors = false;
  float sparklineLowThreshold = NAN;
  float sparklineHighThreshold = NAN;
};

class TextAnimator {
 public:
  void start(const String& text, AnimationStyle style, const CRGB& color,
             uint16_t stepMs, uint32_t holdMs,
             TinyIcon icon = TinyIcon::None, uint8_t iconLevel = 50,
             uint32_t customIconId = 0, bool clockFont = false,
             bool scrollIconWithTicker = false);
  void setIconLabel(const String& label);
  void setAnalogClockTime(uint8_t hour, uint8_t minute) {
    analogHour_ = hour % 24U;
    analogMinute_ = minute % 60U;
  }
  void setIconStore(LaMetricIcons* iconStore) { iconStore_ = iconStore; }
  void setSparkline(const float* values, uint8_t count,
                    int8_t markerIndex = -1,
                    bool invertDirectionColors = false,
                    bool blinkMarker = false,
                    bool usePriceBandColors = false,
                    float lowThreshold = NAN,
                    float highThreshold = NAN);
  void render(PixelDisplay& display, uint32_t nowMs);
  bool finished(uint32_t nowMs) const;
  uint32_t durationMs() const;
  bool running() const { return running_; }
  uint32_t sceneKey() const { return running_ ? startedMs_ : 0; }
  bool browserTickerScene(uint32_t nowMs, BrowserTickerScene& scene) const;
  void stop() { running_ = false; }
  void replay(uint32_t nowMs) {
    startedMs_ = nowMs;
    transitionPage_ = UINT16_MAX;
  }
  void pauseFor(uint32_t durationMs) {
    if (running_) startedMs_ += durationMs;
  }

 private:
  const String& pageText(uint16_t page) const;
  const String& flipText(uint32_t elapsedMs, uint16_t page);
  uint32_t pageDurationMs() const;
  void preparePixelTransition(PixelDisplay& display, uint16_t page);
  void renderRain(PixelDisplay& display, uint32_t inPageMs) const;
  void renderMagic(PixelDisplay& display, uint32_t inPageMs) const;
  void renderWideSparkline(PixelDisplay& display, uint32_t elapsedMs) const;
  void buildPages();
  int16_t renderedWidth(const String& text) const;
  Glyph renderedGlyph(uint32_t codepoint) const;
  bool hasIcon() const {
    return icon_ != TinyIcon::None || customIconId_ != 0 ||
           !iconLabel_.isEmpty();
  }
  void drawRenderedText(PixelDisplay& display, int16_t x, int16_t y,
                        const String& text, const CRGB& color) const;
  void drawRenderedTextSubpixel(PixelDisplay& display, int32_t x256,
                                int16_t y, const String& text,
                                const CRGB& color) const;
  int16_t contentLeft() const {
    if (style_ == AnimationStyle::Ticker && scrollIconWithTicker_) {
      return 0;
    }
    return sparklineCount_ >= 2 || hasIcon()
               ? 9
               : 0;
  }
  int16_t contentWidth() const {
    return hardware::kMatrixWidth - contentLeft();
  }
  void renderIcon(PixelDisplay& display, uint32_t elapsedMs) const;
  void renderIconAt(PixelDisplay& display, int16_t x, uint32_t elapsedMs,
                    bool clearArea) const;
  void renderIconAtSubpixel(PixelDisplay& display, int32_t x256,
                            uint32_t elapsedMs) const;
  uint32_t elapsedSinceStart(uint32_t nowMs) const;
  uint32_t baseDurationMs() const;
  uint32_t chartDurationMs() const;

  String text_;
  String previousText_;
  std::vector<String> pages_;
  AnimationStyle style_ = AnimationStyle::Static;
  CRGB color_ = CRGB::White;
  TinyIcon icon_ = TinyIcon::None;
  TinyIcon previousIcon_ = TinyIcon::None;
  LaMetricIcons* iconStore_ = nullptr;
  uint32_t customIconId_ = 0;
  uint32_t previousCustomIconId_ = 0;
  String iconLabel_;
  String previousIconLabel_;
  bool scrollIconWithTicker_ = false;
  uint8_t iconLevel_ = 50;
  uint8_t analogHour_ = UINT8_MAX;
  uint8_t analogMinute_ = UINT8_MAX;
  uint8_t previousAnalogHour_ = UINT8_MAX;
  uint8_t previousAnalogMinute_ = UINT8_MAX;
  float sparkline_[23]{};
  uint8_t sparklineCount_ = 0;
  int8_t sparklineMarker_ = -1;
  bool sparklineInvertColors_ = false;
  bool sparklineBlinkMarker_ = false;
  bool sparklineBandColors_ = false;
  float sparklineLowThreshold_ = NAN;
  float sparklineHighThreshold_ = NAN;
  uint16_t stepMs_ = 42;
  uint32_t holdMs_ = 10000;
  uint32_t startedMs_ = 0;
  int16_t width_ = 0;
  bool clockFont_ = false;
  uint16_t pageCount_ = 1;
  bool running_ = false;
  CRGB previousFrame_[hardware::kLedCount]{};
  CRGB targetFrame_[hardware::kLedCount]{};
  struct MovePoint {
    int8_t fromX;
    int8_t fromY;
    int8_t toX;
    int8_t toY;
    CRGB color;
  };
  struct GlyphMove {
    int8_t fromX;
    int8_t fromY;
    int8_t toX;
    int8_t toY;
    uint32_t codepoint;
    CRGB color;
  };
  MovePoint moves_[hardware::kLedCount]{};
  GlyphMove glyphMoves_[64]{};
  uint16_t moveCount_ = 0;
  uint8_t glyphMoveCount_ = 0;
  uint16_t transitionPage_ = UINT16_MAX;
  String lastRenderedText_;
  String flipScratch_;
  int16_t lastRenderedX_ = 0;
  bool hasLastRenderedText_ = false;
};

}  // namespace pixelos
