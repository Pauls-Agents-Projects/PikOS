#pragma once

#include <Arduino.h>

namespace pixelos {

struct Glyph {
  uint8_t width;
  uint8_t rows[7];
};

uint32_t nextCodepoint(const String& text, size_t& offset);
Glyph glyphFor(uint32_t codepoint);
Glyph clockGlyphFor(uint32_t codepoint);
int16_t measureText(const String& text);
int16_t measureClockText(const String& text);
String uppercaseForDisplay(const String& text);

}  // namespace pixelos
