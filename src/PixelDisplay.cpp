#include "PixelDisplay.h"

#include "PixelFont.h"

namespace pixelos {

void PixelDisplay::begin() {
  FastLED.addLeds<WS2812B, hardware::kMatrixPin, GRB>(leds_,
                                                       hardware::kLedCount);
  FastLED.setMaxPowerInVoltsAndMilliamps(5, 2000);
  // Icons and the browser twin use literal sRGB channel values. Applying
  // TypicalLEDStrip here silently attenuated green more than blue, turning
  // neutral greys violet and reds pink as brightness rose. Brightness is a
  // separate global scalar; leave the source colours untouched.
  FastLED.setCorrection(UncorrectedColor);
  // Dithering needs repeated refreshes even for a static frame. The TC001
  // holds PWM values itself, so disabling it lets us skip identical frames
  // and keeps low-level colours stable instead of temporally mixing channels.
  FastLED.setDither(DISABLE_DITHER);
  setBrightness(brightness_);
  clear();
  show();
}

void PixelDisplay::clear() {
  fill_solid(leds_, hardware::kLedCount, CRGB::Black);
}

uint16_t PixelDisplay::physicalIndex(int16_t x, int16_t y) const {
  if (orientation180_) {
    x = hardware::kMatrixWidth - 1 - x;
    y = hardware::kMatrixHeight - 1 - y;
  }

  // TC001 is wired as one 32x8 row-major matrix. Alternate rows run in
  // opposite directions along the physical WS2812B chain.
  return static_cast<uint16_t>(y) * hardware::kMatrixWidth +
         ((y & 1) ? hardware::kMatrixWidth - 1 - x : x);
}

void PixelDisplay::setPixel(int16_t x, int16_t y, const CRGB& color) {
  if (x < 0 || x >= hardware::kMatrixWidth || y < 0 ||
      y >= hardware::kMatrixHeight) {
    return;
  }
  leds_[physicalIndex(x, y)] = color;
}

CRGB PixelDisplay::getPixel(int16_t x, int16_t y) const {
  if (x < 0 || x >= hardware::kMatrixWidth || y < 0 ||
      y >= hardware::kMatrixHeight) {
    return CRGB::Black;
  }
  return leds_[physicalIndex(x, y)];
}

void PixelDisplay::drawText(int16_t x, int16_t y, const String& text,
                            const CRGB& color) {
  size_t offset = 0;
  int16_t cursor = x;
  while (offset < text.length()) {
    const Glyph glyph = glyphFor(nextCodepoint(text, offset));
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < glyph.width; ++column) {
        const uint8_t bit = glyph.width - 1 - column;
        if ((glyph.rows[row] >> bit) & 0x01) {
          setPixel(cursor + column, y + row, color);
        }
      }
    }
    cursor += glyph.width + 1;
  }
}

void PixelDisplay::drawClockText(int16_t x, int16_t y, const String& text,
                                 const CRGB& color) {
  size_t offset = 0;
  int16_t cursor = x;
  while (offset < text.length()) {
    const Glyph glyph = clockGlyphFor(nextCodepoint(text, offset));
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < glyph.width; ++column) {
        const uint8_t bit = glyph.width - 1 - column;
        if ((glyph.rows[row] >> bit) & 0x01) {
          setPixel(cursor + column, y + row, color);
        }
      }
    }
    cursor += glyph.width + 1;
  }
}

void PixelDisplay::drawRainbowText(int16_t x, int16_t y, const String& text,
                                   uint8_t startHue) {
  size_t offset = 0;
  int16_t cursor = x;
  while (offset < text.length()) {
    const Glyph glyph = glyphFor(nextCodepoint(text, offset));
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < glyph.width; ++column) {
        const uint8_t bit = glyph.width - 1 - column;
        if ((glyph.rows[row] >> bit) & 0x01) {
          setPixel(cursor + column, y + row,
                   CHSV(startHue + static_cast<uint8_t>(
                                       (cursor + column) * 7),
                        210, 255));
        }
      }
    }
    cursor += glyph.width + 1;
  }
}

void PixelDisplay::drawClockRainbowText(int16_t x, int16_t y,
                                        const String& text,
                                        uint8_t startHue) {
  size_t offset = 0;
  int16_t cursor = x;
  while (offset < text.length()) {
    const Glyph glyph = clockGlyphFor(nextCodepoint(text, offset));
    for (uint8_t row = 0; row < 7; ++row) {
      for (uint8_t column = 0; column < glyph.width; ++column) {
        const uint8_t bit = glyph.width - 1 - column;
        if ((glyph.rows[row] >> bit) & 0x01) {
          setPixel(cursor + column, y + row,
                   CHSV(startHue + static_cast<uint8_t>(
                                       (cursor + column) * 7),
                        210, 255));
        }
      }
    }
    cursor += glyph.width + 1;
  }
}

void PixelDisplay::drawAnalogClock(int16_t x, int16_t y,
                                   const CRGB& primary, uint8_t hour,
                                   uint8_t minute) {
  auto pixel = [this, x, y](int8_t px, int8_t py, const CRGB& color) {
    setPixel(x + px, y + py, color);
  };
  CRGB face = primary;
  face.nscale8_video(118);

  // Full 8x8 outline. The deliberately open corners preserve the impression
  // of a round dial without sacrificing the interior pixels needed by hands.
  for (int8_t px = 2; px <= 5; ++px) {
    pixel(px, 0, face);
    pixel(px, 7, face);
  }
  for (int8_t py = 2; py <= 5; ++py) {
    pixel(0, py, face);
    pixel(7, py, face);
  }
  pixel(1, 1, face);
  pixel(6, 1, face);
  pixel(1, 6, face);
  pixel(6, 6, face);

  // Twelve discrete inner-dial endpoints around a (3,3) pivot. They are
  // intentionally inside the outline so a hand never erases the face.
  static constexpr int8_t kHandPoints[12][2] = {
      {3, 1}, {4, 1}, {5, 2}, {5, 3}, {5, 4}, {4, 5},
      {3, 6}, {2, 5}, {1, 4}, {1, 3}, {1, 2}, {2, 1}};
  auto line = [&pixel](int8_t x0, int8_t y0, int8_t x1, int8_t y1,
                       const CRGB& color) {
    const int8_t dx = abs(x1 - x0);
    const int8_t sx = x0 < x1 ? 1 : -1;
    const int8_t dy = -abs(y1 - y0);
    const int8_t sy = y0 < y1 ? 1 : -1;
    int8_t error = dx + dy;
    while (true) {
      pixel(x0, y0, color);
      if (x0 == x1 && y0 == y1) break;
      const int8_t twice = error * 2;
      if (twice >= dy) {
        error += dy;
        x0 += sx;
      }
      if (twice <= dx) {
        error += dx;
        y0 += sy;
      }
    }
  };

  minute %= 60;
  const uint8_t minuteIndex = static_cast<uint8_t>((minute + 2U) / 5U) % 12U;
  const uint8_t hourIndex = static_cast<uint8_t>(
      (((hour % 12U) * 60U + minute + 30U) / 60U) % 12U);
  const int8_t hourX = static_cast<int8_t>(
      3 + (kHandPoints[hourIndex][0] - 3) / 2);
  const int8_t hourY = static_cast<int8_t>(
      3 + (kHandPoints[hourIndex][1] - 3) / 2);
  line(3, 3, hourX, hourY, primary);
  line(3, 3, kHandPoints[minuteIndex][0],
       kHandPoints[minuteIndex][1], CRGB::White);
  pixel(3, 3, CRGB::White);
}

void PixelDisplay::drawIcon(TinyIcon icon, int16_t x, int16_t y,
                            const CRGB& primary, uint32_t phaseMs,
                            uint8_t level) {
  const CRGB accent = CRGB::White;
  auto pixel = [this, x, y](int8_t px, int8_t py, const CRGB& color) {
    setPixel(x + px, y + py, color);
  };
  auto horizontal =
      [&pixel](int8_t x1, int8_t x2, int8_t py, const CRGB& color) {
        for (int8_t px = x1; px <= x2; ++px) {
          pixel(px, py, color);
        }
      };

  switch (icon) {
    case TinyIcon::Clock: {
      horizontal(1, 3, 0, primary);
      horizontal(1, 3, 6, primary);
      for (int8_t py = 1; py <= 5; ++py) {
        pixel(0, py, primary);
        pixel(4, py, primary);
      }
      pixel(2, 3, accent);
      pixel(2, 2, accent);
      if ((phaseMs / 1000) % 2 == 0) {
        pixel(3, 3, accent);
      } else {
        pixel(2, 4, accent);
      }
      break;
    }
    case TinyIcon::Hourglass: {
      horizontal(0, 5, 0, accent);
      horizontal(0, 5, 7, accent);
      pixel(1, 1, accent);
      pixel(4, 1, accent);
      pixel(1, 2, accent);
      pixel(4, 2, accent);
      pixel(2, 3, accent);
      pixel(3, 3, accent);
      pixel(2, 4, accent);
      pixel(3, 4, accent);
      pixel(1, 5, accent);
      pixel(4, 5, accent);
      pixel(1, 6, accent);
      pixel(4, 6, accent);
      const bool falling = (phaseMs / 350U) % 2U == 0U;
      horizontal(2, 3, 2, primary);
      horizontal(2, 3, 5, primary);
      pixel(falling ? 2 : 3, falling ? 4 : 3, primary);
      break;
    }
    case TinyIcon::Thermometer: {
      pixel(1, 0, accent);
      pixel(2, 0, accent);
      pixel(3, 0, accent);
      for (int8_t py = 1; py <= 4; ++py) {
        pixel(1, py, accent);
        pixel(3, py, accent);
      }
      horizontal(0, 4, 5, accent);
      horizontal(1, 3, 6, accent);
      pixel(2, 7, accent);
      // The first of five temperature bands fills the plus-shaped bulb. Each
      // warmer band adds one of the four remaining column pixels above it.
      // The outer ring stays one pixel wide and white all the way around.
      const uint8_t fillRows =
          level == 0 ? 0 : min<uint8_t>(5, (level + 19) / 20);
      if (fillRows != 0) {
        horizontal(1, 3, 5, primary);
        pixel(2, 6, primary);
        for (uint8_t row = 1; row < fillRows; ++row) {
          pixel(2, 5 - row, primary);
        }
      }
      break;
    }
    case TinyIcon::Weather: {
      const bool alternate = (phaseMs / 700) % 2;
      pixel(1, 0, primary);
      pixel(0, 1, primary);
      pixel(2, 1, primary);
      if (alternate) {
        pixel(0, 0, primary);
      }
      horizontal(1, 4, 3, accent);
      pixel(0, 4, accent);
      pixel(5, 4, accent);
      horizontal(1, 4, 5, accent);
      pixel(1, 6, primary);
      if (alternate) {
        pixel(4, 6, primary);
      }
      break;
    }
    case TinyIcon::Wind: {
      horizontal(0, 4, 1, primary);
      pixel(5, 0, primary);
      pixel(6, 1, primary);
      pixel(5, 2, primary);
      horizontal(0, 5, 3, accent);
      pixel(6, 4, accent);
      pixel(5, 5, accent);
      horizontal(0, 3, 6, primary);
      break;
    }
    case TinyIcon::Rain: {
      horizontal(2, 5, 2, accent);
      pixel(1, 3, accent);
      pixel(6, 3, accent);
      horizontal(1, 6, 4, accent);
      const bool alternate = (phaseMs / 350U) % 2U == 0U;
      pixel(2, alternate ? 5 : 6, primary);
      pixel(4, alternate ? 6 : 5, primary);
      pixel(6, alternate ? 5 : 6, primary);
      break;
    }
    case TinyIcon::Transit: {
      horizontal(1, 6, 0, primary);
      pixel(0, 1, primary);
      pixel(7, 1, primary);
      for (int8_t py = 2; py <= 5; ++py) {
        pixel(0, py, primary);
        pixel(7, py, primary);
      }
      horizontal(1, 6, 5, primary);
      horizontal(1, 6, 6, accent);
      horizontal(1, 3, 2, accent);
      horizontal(4, 6, 2, accent);
      pixel(2, 7, primary);
      pixel(5, 7, primary);
      break;
    }
    case TinyIcon::Rss:
      pixel(0, 6, primary);
      pixel(0, 3, primary);
      pixel(1, 4, primary);
      pixel(2, 5, primary);
      pixel(3, 6, primary);
      pixel(0, 0, primary);
      pixel(1, 1, primary);
      pixel(2, 2, primary);
      pixel(3, 3, primary);
      pixel(4, 4, primary);
      pixel(5, 5, primary);
      break;
    case TinyIcon::Battery: {
      // Upright 6x8 battery: its five interior rows map directly to five
      // 20%-wide charge levels. The larger fill is much easier to read than
      // the old three-column horizontal symbol from across a room.
      horizontal(3, 4, 0, accent);
      horizontal(1, 6, 1, accent);
      horizontal(1, 6, 7, accent);
      for (int8_t py = 2; py <= 6; ++py) {
        pixel(1, py, accent);
        pixel(6, py, accent);
      }
      const uint8_t filled =
          level == 0 ? 0 : min<uint8_t>(5, (level + 19) / 20);
      for (uint8_t row = 0; row < filled; ++row) {
        horizontal(2, 5, 6 - row, primary);
      }
      break;
    }
    case TinyIcon::Calendar: {
      // White calendar sheet with a red live day-of-month. This reverses the
      // earlier white-on-black date treatment and stays legible at low LED
      // brightness while keeping the familiar red LaMetric-style header.
      const CRGB header(235, 42, 48);
      horizontal(0, 7, 1, header);
      horizontal(0, 7, 2, header);
      for (uint8_t sheetRow = 3; sheetRow < 8; ++sheetRow) {
        horizontal(0, 7, sheetRow, accent);
      }
      pixel(1, 0, accent);
      pixel(6, 0, accent);
      static const uint8_t digits[10][5] = {
          {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7},
          {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
          {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7},
          {7, 5, 7, 1, 7}};
      const uint8_t day = constrain(level, 1, 31);
      const bool twoDigits = day >= 10;
      const uint8_t first = twoDigits ? day / 10 : day;
      const uint8_t second = day % 10;
      const int8_t firstX = twoDigits ? 0 : 2;
      for (uint8_t row = 0; row < 5; ++row) {
        for (uint8_t column = 0; column < 3; ++column) {
          if (digits[first][row] & (4 >> column))
            pixel(firstX + column, 3 + row, header);
          if (twoDigits && (digits[second][row] & (4 >> column)))
            pixel(4 + column, 3 + row, header);
        }
      }
      break;
    }
    case TinyIcon::Activity: {
      // A compact running shoe fallback. The moving cyan sole makes the
      // built-in icon identifiable without requiring a gallery download.
      const bool stride = (phaseMs / 240U) % 2U;
      pixel(1, 1, accent);
      pixel(2, 2, accent);
      pixel(2, 3, primary);
      pixel(3, 3, primary);
      pixel(3, 4, primary);
      pixel(4, 4, primary);
      pixel(5, 5, primary);
      pixel(6, 5, primary);
      horizontal(1, 6, 6, primary);
      horizontal(stride ? 2 : 1, stride ? 6 : 5, 7, accent);
      break;
    }
    case TinyIcon::Reddit: {
      horizontal(1, 5, 2, primary);
      pixel(0, 3, primary);
      pixel(6, 3, primary);
      horizontal(1, 5, 6, primary);
      pixel(1, 5, primary);
      pixel(5, 5, primary);
      pixel(2, 3, accent);
      pixel(4, 3, accent);
      horizontal(2, 4, 5, accent);
      pixel(4, 0, primary);
      pixel(5, 0, primary);
      pixel(4, 1, primary);
      break;
    }
    case TinyIcon::None:
    default:
      break;
  }
}

void PixelDisplay::drawCentered(const String& text, const CRGB& color,
                                int16_t y) {
  const int16_t width = measureText(text);
  drawText((hardware::kMatrixWidth - width) / 2, y, text, color);
}

void PixelDisplay::show() {
  // A WS2812 transmission for all 256 pixels occupies the CPU for roughly
  // 7.7 ms. Most PikOS render ticks produce an identical static frame; avoid
  // spending almost a third of the main loop retransmitting it.
  uint32_t hash = 2166136261UL;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(leds_);
  for (size_t index = 0; index < sizeof(leds_); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  if (!forceShow_ && hash == lastFrameHash_) {
    ++skippedFrames_;
    return;
  }
  const uint32_t startedUs = micros();
  FastLED.show();
  const uint32_t showUs = static_cast<uint32_t>(micros() - startedUs);
  maximumShowUs_ = max<uint32_t>(maximumShowUs_, showUs);
  lastFrameHash_ = hash;
  forceShow_ = false;
  ++transmittedFrames_;
  ++frameSequence_;
}

void PixelDisplay::showFrame(const CRGB* logicalFrame) {
  for (uint8_t y = 0; y < hardware::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
      setPixel(x, y, logicalFrame[y * hardware::kMatrixWidth + x]);
    }
  }
  show();
}

void PixelDisplay::setBrightness(uint8_t value) {
  if (brightness_ == value) return;
  brightness_ = value;
  FastLED.setBrightness(value);
  forceShow_ = true;
}

void PixelDisplay::setOrientation(bool orientation180) {
  if (orientation180_ == orientation180) return;
  orientation180_ = orientation180;
  forceShow_ = true;
}

}  // namespace pixelos
