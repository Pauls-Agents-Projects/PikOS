#include "TextAnimator.h"

#include "Hardware.h"
#include "LaMetricIcons.h"
#include "PixelFont.h"

namespace pixelos {
namespace {

void appendCodepoint(String& output, uint32_t codepoint) {
  if (codepoint < 0x80) {
    output += static_cast<char>(codepoint);
  } else if (codepoint < 0x800) {
    output += static_cast<char>(0xc0 | (codepoint >> 6));
    output += static_cast<char>(0x80 | (codepoint & 0x3f));
  } else {
    output += static_cast<char>(0xe0 | (codepoint >> 12));
    output += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    output += static_cast<char>(0x80 | (codepoint & 0x3f));
  }
}

int16_t subpixelFloor(int32_t value256) {
  return value256 >= 0
             ? static_cast<int16_t>(value256 / 256)
             : static_cast<int16_t>(-((-value256 + 255) / 256));
}

void addScaledPixel(PixelDisplay& display, int16_t x, int16_t y,
                    const CRGB& color, uint8_t amount) {
  if (amount == 0) return;
  CRGB contribution = color;
  if (amount != 255) {
    contribution.nscale8(amount);
  }
  CRGB blended = display.getPixel(x, y);
  blended += contribution;
  display.setPixel(x, y, blended);
}

}  // namespace

AnimationStyle parseAnimationStyle(const String& value) {
  if (value == "ticker") {
    return AnimationStyle::Ticker;
  }
  if (value == "flip") {
    return AnimationStyle::Flip;
  }
  if (value == "slide") {
    return AnimationStyle::Slide;
  }
  if (value == "rainbow") {
    return AnimationStyle::Rainbow;
  }
  if (value == "rain") {
    return AnimationStyle::Rain;
  }
  if (value == "magic") {
    return AnimationStyle::Magic;
  }
  return AnimationStyle::Static;
}

const char* animationStyleName(AnimationStyle style) {
  switch (style) {
    case AnimationStyle::Ticker:
      return "ticker";
    case AnimationStyle::Flip:
      return "flip";
    case AnimationStyle::Slide:
      return "slide";
    case AnimationStyle::Rainbow:
      return "rainbow";
    case AnimationStyle::Rain:
      return "rain";
    case AnimationStyle::Magic:
      return "magic";
    case AnimationStyle::Static:
    default:
      return "static";
  }
}

void TextAnimator::start(const String& text, AnimationStyle style,
                         const CRGB& color, uint16_t stepMs,
                         uint32_t holdMs, TinyIcon icon,
                         uint8_t iconLevel, uint32_t customIconId,
                         bool clockFont, bool scrollIconWithTicker) {
  previousText_ = text_;
  previousIcon_ = icon_;
  previousCustomIconId_ = customIconId_;
  previousIconLabel_ = iconLabel_;
  previousAnalogHour_ = analogHour_;
  previousAnalogMinute_ = analogMinute_;
  text_ = uppercaseForDisplay(text);
  // render() runs alongside the HTTPS worker. Reserve its reusable strings at
  // start time so split-flap frames never fragment the TLS heap.
  flipScratch_.reserve(text_.length() + 4);
  lastRenderedText_.reserve(text_.length() + 4);
  style_ = style;
  color_ = color;
  icon_ = icon;
  customIconId_ = customIconId;
  iconLabel_ = "";
  scrollIconWithTicker_ = scrollIconWithTicker;
  iconLevel_ = iconLevel;
  analogHour_ = UINT8_MAX;
  analogMinute_ = UINT8_MAX;
  clockFont_ = clockFont;
  sparklineCount_ = 0;
  sparklineMarker_ = -1;
  sparklineInvertColors_ = false;
  sparklineBlinkMarker_ = false;
  sparklineBandColors_ = false;
  sparklineLowThreshold_ = NAN;
  sparklineHighThreshold_ = NAN;
  stepMs_ = max<uint16_t>(stepMs, 10);
  holdMs_ = max<uint32_t>(holdMs, 250);
  startedMs_ = millis();
  width_ = renderedWidth(text_);
  buildPages();
  pageCount_ = static_cast<uint16_t>(pages_.size());
  transitionPage_ = UINT16_MAX;
  moveCount_ = 0;
  glyphMoveCount_ = 0;
  running_ = true;
}

void TextAnimator::setIconLabel(const String& label) {
  iconLabel_ = uppercaseForDisplay(label);
  buildPages();
  pageCount_ = static_cast<uint16_t>(pages_.size());
}

bool TextAnimator::browserTickerScene(uint32_t nowMs,
                                      BrowserTickerScene& scene) const {
  if (!running_) return false;
  scene.text = text_;
  scene.previousText = previousText_;
  scene.color = color_;
  scene.style = style_;
  scene.key = startedMs_;
  scene.elapsedMs = elapsedSinceStart(nowMs);
  scene.stepMs = stepMs_;
  scene.holdMs = holdMs_;
  scene.baseDurationMs = baseDurationMs();
  scene.durationMs = durationMs();
  scene.pageCount = pageCount_;
  scene.textWidth = width_;
  scene.customIconId = customIconId_;
  scene.icon = icon_;
  scene.iconLevel = iconLevel_;
  scene.analogHour = analogHour_;
  scene.analogMinute = analogMinute_;
  scene.iconLabel = iconLabel_;
  scene.clockFont = clockFont_;
  scene.linkedIcon = scrollIconWithTicker_;
  scene.sparklineCount = sparklineCount_;
  scene.sparklineMarker = sparklineMarker_;
  scene.sparklineBlinkMarker = sparklineBlinkMarker_;
  scene.sparklineBandColors = sparklineBandColors_;
  scene.sparklineLowThreshold = sparklineLowThreshold_;
  scene.sparklineHighThreshold = sparklineHighThreshold_;
  for (uint8_t index = 0; index < sparklineCount_; ++index) {
    scene.sparkline[index] = sparkline_[index];
  }
  return true;
}

int16_t TextAnimator::renderedWidth(const String& text) const {
  return clockFont_ ? measureClockText(text) : measureText(text);
}

Glyph TextAnimator::renderedGlyph(uint32_t codepoint) const {
  return clockFont_ ? clockGlyphFor(codepoint) : glyphFor(codepoint);
}

void TextAnimator::drawRenderedText(PixelDisplay& display, int16_t x,
                                    int16_t y, const String& text,
                                    const CRGB& color) const {
  if (clockFont_) {
    display.drawClockText(x, y, text, color);
  } else {
    display.drawText(x, y, text, color);
  }
}

void TextAnimator::drawRenderedTextSubpixel(PixelDisplay& display,
                                            int32_t x256, int16_t y,
                                            const String& text,
                                            const CRGB& color) const {
  const int16_t baseX = subpixelFloor(x256);
  const uint8_t fraction = static_cast<uint8_t>(x256 - baseX * 256);
  const uint8_t leftWeight =
      fraction == 0 ? 255 : static_cast<uint8_t>(255 - fraction);
  int16_t cursor = baseX;
  size_t offset = 0;
  while (offset < text.length()) {
    const Glyph glyph = renderedGlyph(nextCodepoint(text, offset));
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < glyph.width; ++column) {
        const uint8_t bit = glyph.width - 1 - column;
        if (((glyph.rows[row] >> bit) & 1U) == 0) continue;
        addScaledPixel(display, cursor + column, y + row, color,
                       leftWeight);
        if (fraction != 0) {
          addScaledPixel(display, cursor + column + 1, y + row, color,
                         fraction);
        }
      }
    }
    cursor += glyph.width + 1;
  }
}

void TextAnimator::setSparkline(const float* values, uint8_t count,
                                int8_t markerIndex,
                                bool invertDirectionColors,
                                bool blinkMarker,
                                bool usePriceBandColors,
                                float lowThreshold,
                                float highThreshold) {
  sparklineCount_ = min<uint8_t>(count, 23);
  for (uint8_t index = 0; index < sparklineCount_; ++index) {
    sparkline_[index] = values[index];
  }
  sparklineMarker_ =
      markerIndex >= 0 && markerIndex < sparklineCount_ ? markerIndex : -1;
  sparklineInvertColors_ = invertDirectionColors;
  sparklineBlinkMarker_ = blinkMarker;
  sparklineBandColors_ =
      usePriceBandColors && !isnan(lowThreshold) && !isnan(highThreshold);
  sparklineLowThreshold_ = lowThreshold;
  sparklineHighThreshold_ = highThreshold;
  buildPages();
  pageCount_ = static_cast<uint16_t>(pages_.size());
  transitionPage_ = UINT16_MAX;
}

uint32_t TextAnimator::elapsedSinceStart(uint32_t nowMs) const {
  // start() reads millis() internally. The caller may have captured nowMs a
  // fraction earlier, so unsigned subtraction must not turn that first frame
  // into a 49-day-old animation.
  const int32_t delta = static_cast<int32_t>(nowMs - startedMs_);
  return delta < 0 ? 0 : static_cast<uint32_t>(delta);
}

const String& TextAnimator::pageText(uint16_t page) const {
  static const String empty;
  if (pages_.empty()) {
    return empty;
  }
  return pages_[min<size_t>(page, pages_.size() - 1)];
}

void TextAnimator::buildPages() {
  pages_.clear();
  String line;
  String word;

  auto placeWord = [this, &line](const String& value) {
    if (value.isEmpty()) {
      return;
    }
    String candidate = line.isEmpty() ? value : line + " " + value;
    if (renderedWidth(candidate) <= contentWidth()) {
      line = candidate;
      return;
    }
    if (!line.isEmpty()) {
      pages_.push_back(line);
      line = "";
    }
    if (renderedWidth(value) <= contentWidth()) {
      line = value;
      return;
    }

    String chunk;
    size_t offset = 0;
    while (offset < value.length()) {
      const uint32_t point = nextCodepoint(value, offset);
      String encoded;
      appendCodepoint(encoded, point);
      const String expanded = chunk + encoded;
      if (!chunk.isEmpty() &&
          renderedWidth(expanded) > contentWidth()) {
        pages_.push_back(chunk);
        chunk = encoded;
      } else {
        chunk = expanded;
      }
    }
    line = chunk;
  };

  size_t offset = 0;
  while (offset < text_.length()) {
    const uint32_t point = nextCodepoint(text_, offset);
    if (point == ' ') {
      placeWord(word);
      word = "";
    } else {
      appendCodepoint(word, point);
    }
  }
  placeWord(word);
  if (!line.isEmpty()) {
    pages_.push_back(line);
  }
  if (pages_.empty()) {
    pages_.push_back("");
  }
}

void TextAnimator::renderIcon(PixelDisplay& display,
                              uint32_t elapsedMs) const {
  renderIconAt(display, 0, elapsedMs, true);
}

void TextAnimator::renderIconAt(PixelDisplay& display, int16_t x,
                                uint32_t elapsedMs,
                                bool clearArea) const {
  if (icon_ == TinyIcon::None && customIconId_ == 0 &&
      iconLabel_.isEmpty()) {
    return;
  }
  if (clearArea) {
    const int16_t clearWidth =
        max<int16_t>(7, contentLeft());
    for (int16_t clearX = 0; clearX < clearWidth; ++clearX) {
      for (int16_t y = 0; y < hardware::kMatrixHeight; ++y) {
        display.setPixel(clearX, y, CRGB::Black);
      }
    }
  }

  auto drawValue = [this, &display, elapsedMs](
                       TinyIcon icon, uint32_t customIconId,
                       const String& label, uint8_t analogHour,
                       uint8_t analogMinute, int16_t drawX, int16_t drawY) {
    if (customIconId != 0 && iconStore_ != nullptr &&
        iconStore_->draw(customIconId, display, drawX, drawY, elapsedMs)) {
      return;
    }
    if (!label.isEmpty()) {
      const int16_t labelX =
          drawX + (8 - measureText(label)) / 2;
      display.drawText(labelX, drawY, label, color_);
      return;
    }
    if (icon == TinyIcon::Clock && analogHour < 24U &&
        analogMinute < 60U) {
      display.drawAnalogClock(drawX, drawY, color_, analogHour,
                              analogMinute);
      return;
    }
    display.drawIcon(icon, drawX, drawY, color_, elapsedMs, iconLevel_);
  };

  const bool iconChanged =
      previousIcon_ != icon_ ||
      previousCustomIconId_ != customIconId_ ||
      previousIconLabel_ != iconLabel_ ||
      previousAnalogHour_ != analogHour_ ||
      previousAnalogMinute_ != analogMinute_;
  if (style_ == AnimationStyle::Flip && iconChanged && elapsedMs < 650) {
    CRGB previousPixels[64]{};
    CRGB targetPixels[64]{};
    auto clearIconArea = [&display, x]() {
      for (int16_t column = x; column < x + 8; ++column) {
        for (uint8_t row = 0; row < 8; ++row) {
          display.setPixel(column, row, CRGB::Black);
        }
      }
    };
    clearIconArea();
    drawValue(previousIcon_, previousCustomIconId_, previousIconLabel_,
              previousAnalogHour_, previousAnalogMinute_, x, 0);
    for (uint8_t row = 0; row < 8; ++row) {
      for (uint8_t column = 0; column < 8; ++column) {
        previousPixels[row * 8 + column] =
            display.getPixel(x + column, row);
      }
    }
    clearIconArea();
    drawValue(icon_, customIconId_, iconLabel_, analogHour_, analogMinute_, x,
              0);
    for (uint8_t row = 0; row < 8; ++row) {
      for (uint8_t column = 0; column < 8; ++column) {
        targetPixels[row * 8 + column] = display.getPixel(x + column, row);
      }
    }
    clearIconArea();
    for (uint8_t row = 0; row < 8; ++row) {
      for (uint8_t column = 0; column < 8; ++column) {
        const uint8_t index = row * 8 + column;
        const uint32_t settleAt =
            250 + (column * 53 + row * 71) % 330;
        const bool settled = elapsedMs >= settleAt;
        const bool showIncoming =
            settled ||
            (elapsedMs > 90 &&
             ((elapsedMs / 55 + column * 3 + row * 5) & 1U));
        display.setPixel(x + column, row,
                         showIncoming ? targetPixels[index]
                                      : previousPixels[index]);
      }
    }
    return;
  }
  drawValue(icon_, customIconId_, iconLabel_, analogHour_, analogMinute_, x,
            0);
}

void TextAnimator::renderIconAtSubpixel(PixelDisplay& display,
                                        int32_t x256,
                                        uint32_t elapsedMs) const {
  const int16_t baseX = subpixelFloor(x256);
  const uint8_t fraction = static_cast<uint8_t>(x256 - baseX * 256);
  if (customIconId_ != 0 && iconStore_ != nullptr) {
    CRGB pixels[64]{};
    if (iconStore_->copyFrame(customIconId_, pixels, elapsedMs)) {
      const uint8_t leftWeight =
          fraction == 0 ? 255 : static_cast<uint8_t>(255 - fraction);
      for (uint8_t row = 0; row < 8; ++row) {
        for (uint8_t column = 0; column < 8; ++column) {
          const CRGB color = pixels[row * 8 + column];
          if (color == CRGB::Black) continue;
          addScaledPixel(display, baseX + column, row, color, leftWeight);
          if (fraction != 0) {
            addScaledPixel(display, baseX + column + 1, row, color,
                           fraction);
          }
        }
      }
      return;
    }
  }
  const int16_t roundedX =
      baseX + (fraction >= 128 ? 1 : 0);
  renderIconAt(display, roundedX, elapsedMs, false);
}

const String& TextAnimator::flipText(uint32_t elapsedMs, uint16_t page) {
  const String& points = pageText(page);
  flipScratch_ = "";
  size_t pointOffset = 0;
  size_t previousOffset = 0;
  size_t index = 0;
  while (pointOffset < points.length()) {
    const uint32_t point = nextCodepoint(points, pointOffset);
    const bool hasPrevious = previousOffset < previousText_.length();
    const uint32_t previousPoint =
        hasPrevious ? nextCodepoint(previousText_, previousOffset) : 0;
    const uint32_t settleAt = 280 + index * 95;
    const bool unchanged =
        hasPrevious && previousPoint == point;
    if (unchanged || elapsedMs >= settleAt || point == ' ' ||
        point == ':' || point == '.' || point == '-' || point == '%') {
      appendCodepoint(flipScratch_, point);
    } else {
      const uint32_t cycle = elapsedMs / 55;
      const char randomLetter = clockFont_
                                    ? static_cast<char>(
                                          '0' + ((cycle * 11 + index * 7 +
                                                  page * 3) %
                                                 10))
                                    : static_cast<char>(
                                          'A' + ((cycle * 11 + index * 7 +
                                                  page * 3) %
                                                 26));
      flipScratch_ += randomLetter;
    }
    ++index;
  }
  return flipScratch_;
}

void TextAnimator::renderWideSparkline(PixelDisplay& display,
                                       uint32_t elapsedMs) const {
  constexpr int8_t kLeft = 9;
  constexpr int8_t kWidth = hardware::kMatrixWidth - kLeft;
  float low = sparkline_[0];
  float high = sparkline_[0];
  for (uint8_t index = 1; index < sparklineCount_; ++index) {
    low = min(low, sparkline_[index]);
    high = max(high, sparkline_[index]);
  }
  const float span = max(high - low, 0.0001f);
  auto pointX = [](uint8_t index, uint8_t count) {
    return static_cast<int8_t>(
        kLeft + lroundf(index * (kWidth - 1.0f) / (count - 1)));
  };
  auto pointY = [low, span](float value) {
    return static_cast<int8_t>(
        6 - lroundf((value - low) * 6.0f / span));
  };

  int8_t previousX = pointX(0, sparklineCount_);
  int8_t previousY = pointY(sparkline_[0]);
  constexpr uint32_t kRevealMs = 900;
  const uint32_t chartElapsed =
      elapsedMs > baseDurationMs() ? elapsedMs - baseDurationMs() : 0;
  const uint8_t visibleSegments = min<uint16_t>(
      sparklineCount_ - 1,
      1 + static_cast<uint16_t>(chartElapsed) * (sparklineCount_ - 1) /
              kRevealMs);
  for (uint8_t index = 1; index <= visibleSegments; ++index) {
    const int8_t x = pointX(index, sparklineCount_);
    const int8_t y = pointY(sparkline_[index]);
    const float delta = sparkline_[index] - sparkline_[index - 1];
    CRGB segmentColor(255, 190, 0);
    if (sparklineBandColors_) {
      // Price bands use the same colour language as the energy icon:
      // cheap is green, middle is yellow, expensive is red.
      const float representative =
          (sparkline_[index - 1] + sparkline_[index]) * 0.5f;
      if (representative <= sparklineLowThreshold_) {
        segmentColor = CRGB(0, 255, 102);
      } else if (representative >= sparklineHighThreshold_) {
        segmentColor = CRGB(255, 36, 0);
      }
    } else if (delta > span * 0.002f) {
      segmentColor = sparklineInvertColors_ ? CRGB(255, 36, 0)
                                           : CRGB(0, 255, 102);
    } else if (delta < -span * 0.002f) {
      segmentColor = sparklineInvertColors_ ? CRGB(0, 255, 102)
                                           : CRGB(255, 36, 0);
    }
    // Interpolate over the greater axis so abrupt vertical changes remain a
    // connected line rather than two isolated endpoints.
    const int8_t steps =
        max<int8_t>(1, max<int8_t>(abs(x - previousX),
                                   abs(y - previousY)));
    for (int8_t step = 0; step <= steps; ++step) {
      const int8_t lineX =
          previousX + (x - previousX) * step / steps;
      const int8_t lineY =
          previousY + (y - previousY) * step / steps;
      display.setPixel(lineX, lineY, segmentColor);
    }
    previousX = x;
    previousY = y;
  }

  if (visibleSegments >= sparklineCount_ - 1 && sparklineMarker_ >= 0 &&
      sparklineBlinkMarker_ &&
      (elapsedMs / 420) % 2 == 0) {
    display.setPixel(pointX(sparklineMarker_, sparklineCount_),
                     pointY(sparkline_[sparklineMarker_]), CRGB::White);
  }
}

uint32_t TextAnimator::pageDurationMs() const {
  return 700 +
         min<uint32_t>(holdMs_ / max<uint16_t>(pageCount_, 1), 1800);
}

void TextAnimator::preparePixelTransition(PixelDisplay& display,
                                          uint16_t page) {
  for (uint8_t y = 0; y < hardware::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
      const uint16_t index = y * hardware::kMatrixWidth + x;
      previousFrame_[index] = display.getPixel(x, y);
    }
  }

  display.clear();
  const String& value = pageText(page);
  const int16_t centered =
      contentLeft() + (contentWidth() - renderedWidth(value)) / 2;
  renderIconAt(display, 0, 0, false);
  drawRenderedText(display, centered, 0, value, color_);
  for (uint8_t y = 0; y < hardware::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
      const uint16_t index = y * hardware::kMatrixWidth + x;
      targetFrame_[index] = display.getPixel(x, y);
    }
  }
  display.clear();

  bool usedPrevious[hardware::kLedCount]{};
  bool targetCovered[hardware::kLedCount]{};
  moveCount_ = 0;
  glyphMoveCount_ = 0;

  struct GlyphPosition {
    uint32_t codepoint;
    int16_t x;
    Glyph glyph;
    bool used;
  };
  GlyphPosition oldGlyphs[64]{};
  GlyphPosition newGlyphs[64]{};
  auto collectGlyphs = [this](const String& text, int16_t startX,
                              GlyphPosition* output) -> uint8_t {
    size_t offset = 0;
    int16_t x = startX;
    uint8_t count = 0;
    while (offset < text.length() && count < 64) {
      const uint32_t codepoint = nextCodepoint(text, offset);
      const Glyph glyph = renderedGlyph(codepoint);
      output[count++] = {codepoint, x, glyph, false};
      x += glyph.width + 1;
    }
    return count;
  };
  uint8_t oldGlyphCount = 0;
  if (hasLastRenderedText_) {
    oldGlyphCount =
        collectGlyphs(lastRenderedText_, lastRenderedX_, oldGlyphs);
  }
  const uint8_t newGlyphCount = collectGlyphs(value, centered, newGlyphs);

  // Magic Move first pairs identical glyphs. Their whole 5x7 shape travels
  // together, while unmatched pixels keep the original nearest-pixel effect.
  for (uint8_t incomingIndex = 0; incomingIndex < newGlyphCount;
       ++incomingIndex) {
    GlyphPosition& incoming = newGlyphs[incomingIndex];
    if (incoming.codepoint == ' ' || glyphMoveCount_ >= 64) {
      continue;
    }
    int16_t nearest = -1;
    int16_t nearestDistance = INT16_MAX;
    for (uint8_t index = 0; index < oldGlyphCount; ++index) {
      const GlyphPosition& outgoing = oldGlyphs[index];
      if (outgoing.used || outgoing.codepoint != incoming.codepoint) {
        continue;
      }
      const int16_t distance = abs(incoming.x - outgoing.x);
      if (distance < nearestDistance) {
        nearestDistance = distance;
        nearest = static_cast<int16_t>(index);
      }
    }
    if (nearest < 0) {
      continue;
    }
    GlyphPosition& outgoing = oldGlyphs[nearest];
    outgoing.used = true;
    incoming.used = true;
    glyphMoves_[glyphMoveCount_++] = {
        static_cast<int8_t>(outgoing.x), 0,
        static_cast<int8_t>(incoming.x), 0,
        incoming.codepoint, color_};

    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < outgoing.glyph.width; ++column) {
        const uint8_t bit = outgoing.glyph.width - 1 - column;
        if (((outgoing.glyph.rows[row] >> bit) & 1U) == 0) {
          continue;
        }
        const int16_t x = outgoing.x + column;
        if (x >= contentLeft() && x < hardware::kMatrixWidth) {
          usedPrevious[row * hardware::kMatrixWidth + x] = true;
        }
      }
      for (uint8_t column = 0; column < incoming.glyph.width; ++column) {
        const uint8_t bit = incoming.glyph.width - 1 - column;
        if (((incoming.glyph.rows[row] >> bit) & 1U) == 0) {
          continue;
        }
        const int16_t x = incoming.x + column;
        if (x >= contentLeft() && x < hardware::kMatrixWidth) {
          targetCovered[row * hardware::kMatrixWidth + x] = true;
        }
      }
    }
  }

  for (uint8_t targetY = 0; targetY < 7; ++targetY) {
    for (uint8_t targetX = 0;
         targetX < hardware::kMatrixWidth; ++targetX) {
      const uint16_t targetIndex =
          targetY * hardware::kMatrixWidth + targetX;
      if (targetCovered[targetIndex]) {
        continue;
      }
      const CRGB target =
          targetFrame_[targetIndex];
      if (target == CRGB::Black) {
        continue;
      }

      int16_t nearest = -1;
      uint16_t nearestDistance = UINT16_MAX;
      for (uint8_t oldY = 0; oldY < 7; ++oldY) {
        for (uint8_t oldX = 0;
             oldX < hardware::kMatrixWidth; ++oldX) {
          const uint16_t oldIndex =
              oldY * hardware::kMatrixWidth + oldX;
          if (usedPrevious[oldIndex] ||
              previousFrame_[oldIndex] == CRGB::Black) {
            continue;
          }
          const uint16_t distance =
              abs(static_cast<int>(targetX) - oldX) +
              abs(static_cast<int>(targetY) - oldY);
          if (distance < nearestDistance) {
            nearestDistance = distance;
            nearest = oldIndex;
          }
        }
      }

      MovePoint& move = moves_[moveCount_++];
      move.toX = targetX;
      move.toY = targetY;
      move.color = target;
      if (nearest >= 0) {
        usedPrevious[nearest] = true;
        move.fromX = nearest % hardware::kMatrixWidth;
        move.fromY = nearest / hardware::kMatrixWidth;
      } else {
        move.fromX = targetX;
        move.fromY = -1;
      }
    }
  }
  lastRenderedText_ = value;
  lastRenderedX_ = centered;
  hasLastRenderedText_ = true;
  transitionPage_ = page;
}

void TextAnimator::renderRain(PixelDisplay& display,
                              uint32_t inPageMs) const {
  if (inPageMs < 900) {
    for (uint8_t y = 0; y < hardware::kMatrixHeight; ++y) {
      for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
        const CRGB old =
            previousFrame_[y * hardware::kMatrixWidth + x];
        if (old == CRGB::Black) {
          continue;
        }
        const uint16_t delay = (x * 37 + y * 53) % 250;
        const int16_t fall =
            inPageMs <= delay ? 0 : (inPageMs - delay) / 48;
        display.setPixel(x, y + fall, old);
      }
    }
  }

  if (inPageMs < 350) {
    return;
  }
  const uint32_t incomingMs = inPageMs - 350;
  for (uint8_t y = 0; y < hardware::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
      const CRGB target =
          targetFrame_[y * hardware::kMatrixWidth + x];
      if (target == CRGB::Black) {
        continue;
      }
      const uint16_t delay = (x * 29 + y * 41) % 300;
      if (incomingMs < delay) {
        continue;
      }
      const int16_t steps = min<int16_t>(7, (incomingMs - delay) / 48);
      display.setPixel(x, y - (7 - steps), target);
    }
  }
}

void TextAnimator::renderMagic(PixelDisplay& display,
                               uint32_t inPageMs) const {
  constexpr uint32_t kMoveMs = 820;
  if (inPageMs >= kMoveMs) {
    for (uint8_t y = 0; y < hardware::kMatrixHeight; ++y) {
      for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
        display.setPixel(
            x, y, targetFrame_[y * hardware::kMatrixWidth + x]);
      }
    }
    return;
  }

  for (uint8_t index = 0; index < glyphMoveCount_; ++index) {
    const GlyphMove& move = glyphMoves_[index];
    const int16_t x =
        (move.fromX * static_cast<int32_t>(kMoveMs - inPageMs) +
         move.toX * static_cast<int32_t>(inPageMs)) /
        kMoveMs;
    const int16_t y =
        (move.fromY * static_cast<int32_t>(kMoveMs - inPageMs) +
         move.toY * static_cast<int32_t>(inPageMs)) /
        kMoveMs;
    const Glyph glyph = renderedGlyph(move.codepoint);
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < glyph.width; ++column) {
        const uint8_t bit = glyph.width - 1 - column;
        if ((glyph.rows[row] >> bit) & 1U) {
          CRGB blended = display.getPixel(x + column, y + row);
          blended += move.color;
          display.setPixel(x + column, y + row, blended);
        }
      }
    }
  }

  for (uint16_t index = 0; index < moveCount_; ++index) {
    const MovePoint& move = moves_[index];
    const int16_t x =
        (move.fromX * static_cast<int32_t>(kMoveMs - inPageMs) +
         move.toX * static_cast<int32_t>(inPageMs)) /
        kMoveMs;
    const int16_t y =
        (move.fromY * static_cast<int32_t>(kMoveMs - inPageMs) +
         move.toY * static_cast<int32_t>(inPageMs)) /
        kMoveMs;
    CRGB blended = display.getPixel(x, y);
    blended += move.color;
    display.setPixel(x, y, blended);
  }
}

void TextAnimator::render(PixelDisplay& display, uint32_t nowMs) {
  if (!running_) {
    return;
  }
  const uint32_t elapsed = elapsedSinceStart(nowMs);
  if (sparklineCount_ >= 2 && elapsed >= baseDurationMs()) {
    display.clear();
    renderIcon(display, elapsed);
    renderWideSparkline(display, elapsed);
    return;
  }
  uint32_t transitionSlot = 0;
  uint16_t transitionPage = 0;
  uint32_t transitionInPage = 0;
  if (style_ == AnimationStyle::Rain || style_ == AnimationStyle::Magic) {
    transitionSlot =
        max<uint32_t>(1300, holdMs_ / max<uint16_t>(pageCount_, 1));
    transitionPage =
        min<uint16_t>(elapsed / transitionSlot, pageCount_ - 1);
    // Once the last page has completed, keep it at its settled frame. Using
    // modulo here wrapped a one-page Climate transition back to frame zero,
    // briefly replaying the Time snapshot before every following metric.
    const uint32_t pageStart = transitionPage * transitionSlot;
    transitionInPage = min<uint32_t>(elapsed - pageStart, transitionSlot - 1);
    if (transitionPage_ != transitionPage) {
      preparePixelTransition(display, transitionPage);
    }
  }
  display.clear();

  switch (style_) {
    case AnimationStyle::Ticker: {
      const int32_t x256 =
          (hardware::kMatrixWidth - 1) * 256L -
          static_cast<int32_t>(elapsed * 256UL / stepMs_);
      if (scrollIconWithTicker_) {
        renderIconAtSubpixel(display, x256, elapsed);
        drawRenderedTextSubpixel(display, x256 + 9 * 256L, 0, text_,
                                 color_);
      } else {
        drawRenderedTextSubpixel(display, x256, 0, text_, color_);
        renderIcon(display, elapsed);
      }
      lastRenderedText_ = text_;
      const int16_t wholeX =
          subpixelFloor(x256) + ((x256 & 255) >= 128 ? 1 : 0);
      lastRenderedX_ = scrollIconWithTicker_ ? wholeX + 9 : wholeX;
      hasLastRenderedText_ = true;
      break;
    }
    case AnimationStyle::Flip: {
      const uint32_t pageDuration = pageDurationMs();
      const uint16_t page =
          min<uint16_t>(elapsed / pageDuration, pageCount_ - 1);
      const uint32_t inPage = elapsed % pageDuration;
      const String& value = flipText(inPage, page);
      const int16_t x =
          contentLeft() + (contentWidth() - renderedWidth(value)) / 2;
      drawRenderedText(display, x, 0, value, color_);
      renderIcon(display, elapsed);
      lastRenderedText_ = value;
      lastRenderedX_ = x;
      hasLastRenderedText_ = true;
      break;
    }
    case AnimationStyle::Slide: {
      const uint32_t slot =
          max<uint32_t>(900, holdMs_ / max<uint16_t>(pageCount_, 1));
      const uint16_t page =
          min<uint16_t>(elapsed / slot, pageCount_ - 1);
      const uint32_t inPage = elapsed % slot;
      const String& pageValue = pageText(page);
      const int16_t textWidth = renderedWidth(pageValue);
      const int16_t groupWidth = textWidth + (hasIcon() ? 9 : 0);
      const int16_t centered =
          (hardware::kMatrixWidth - groupWidth) / 2;
      int16_t groupX = centered;
      if (inPage < 260) {
        groupX = hardware::kMatrixWidth -
                 static_cast<int16_t>(
                     (hardware::kMatrixWidth - centered) * inPage / 260);
      } else if (page + 1 < pageCount_ && slot > 520 &&
                 inPage > slot - 260) {
        groupX = centered -
                 static_cast<int16_t>(
                     (inPage - (slot - 260)) *
                     (groupWidth + centered) / 260);
      }
      const int16_t textX = groupX + (hasIcon() ? 9 : 0);
      if (hasIcon()) {
        renderIconAt(display, groupX, elapsed, false);
      }
      drawRenderedText(display, textX, 0, pageValue, color_);
      lastRenderedText_ = pageValue;
      lastRenderedX_ = textX;
      hasLastRenderedText_ = true;
      break;
    }
    case AnimationStyle::Rainbow: {
      const int16_t x =
          width_ > contentWidth()
              ? hardware::kMatrixWidth -
                    static_cast<int16_t>(elapsed / stepMs_)
              : contentLeft() + (contentWidth() - width_) / 2;
      if (clockFont_) {
        display.drawClockRainbowText(x, 0, text_,
                                     static_cast<uint8_t>(elapsed / 12));
      } else {
        display.drawRainbowText(x, 0, text_,
                                static_cast<uint8_t>(elapsed / 12));
      }
      renderIcon(display, elapsed);
      lastRenderedText_ = text_;
      lastRenderedX_ = x;
      hasLastRenderedText_ = true;
      break;
    }
    case AnimationStyle::Rain:
      renderRain(display, transitionInPage);
      if (transitionInPage >= 1000) {
        renderIcon(display, elapsed);
      }
      break;
    case AnimationStyle::Magic:
      renderMagic(display, transitionInPage);
      if (transitionInPage >= 820) {
        renderIcon(display, elapsed);
      }
      break;
    case AnimationStyle::Static:
    default:
      if (pageCount_ > 1) {
        const uint32_t slot =
            max<uint32_t>(500, holdMs_ / pageCount_);
        const String& value =
            pageText(min<uint16_t>(elapsed / slot, pageCount_ - 1));
        const int16_t x =
            contentLeft() + (contentWidth() - renderedWidth(value)) / 2;
        drawRenderedText(display, x, 0, value, color_);
        lastRenderedText_ = value;
        lastRenderedX_ = x;
      } else {
        const int16_t x =
            contentLeft() + (contentWidth() - width_) / 2;
        drawRenderedText(display, x, 0, text_, color_);
        lastRenderedText_ = text_;
        lastRenderedX_ = x;
      }
      hasLastRenderedText_ = true;
      renderIcon(display, elapsed);
      break;
  }
}

bool TextAnimator::finished(uint32_t nowMs) const {
  if (!running_) {
    return true;
  }
  const uint32_t elapsed = elapsedSinceStart(nowMs);
  return elapsed >= durationMs();
}

uint32_t TextAnimator::baseDurationMs() const {
  switch (style_) {
    case AnimationStyle::Ticker: {
      const uint32_t travel =
          static_cast<uint32_t>(
              max<int16_t>(
                  1, width_ + hardware::kMatrixWidth -
                         contentLeft() - 2 +
                         (scrollIconWithTicker_ ? 9 : 0))) *
          stepMs_;
      return travel;
    }
    case AnimationStyle::Flip:
      return static_cast<uint32_t>(pageCount_) * pageDurationMs();
    case AnimationStyle::Rainbow:
      if (width_ > contentWidth()) {
        return max<uint32_t>(
            static_cast<uint32_t>(width_ + contentWidth()) * stepMs_ + 250,
            holdMs_);
      }
      return holdMs_;
    case AnimationStyle::Slide:
    case AnimationStyle::Rain:
    case AnimationStyle::Magic:
    case AnimationStyle::Static:
    default:
      return holdMs_;
  }
}

uint32_t TextAnimator::chartDurationMs() const {
  return sparklineCount_ >= 2
             ? min<uint32_t>(4500, max<uint32_t>(2600, holdMs_ / 3))
             : 0;
}

uint32_t TextAnimator::durationMs() const {
  return baseDurationMs() + chartDurationMs();
}

}  // namespace pixelos
