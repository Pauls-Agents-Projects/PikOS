#pragma once

#include <Arduino.h>

namespace pixelos::hardware {

constexpr uint8_t kMatrixPin = 32;
constexpr uint8_t kMatrixWidth = 32;
constexpr uint8_t kMatrixHeight = 8;
constexpr uint16_t kLedCount = kMatrixWidth * kMatrixHeight;

constexpr uint8_t kButtonLeftPin = 26;
constexpr uint8_t kButtonMiddlePin = 27;
constexpr uint8_t kButtonRightPin = 14;
constexpr uint8_t kResetButtonPin = 13;

constexpr uint8_t kBuzzerPin = 15;
constexpr uint8_t kBatteryPin = 34;
constexpr uint8_t kLightPin = 35;

constexpr uint8_t kI2cSdaPin = 21;
constexpr uint8_t kI2cSclPin = 22;
constexpr uint8_t kSht3xAddress = 0x44;
constexpr uint8_t kRtcAddress = 0x68;

constexpr uint16_t kHttpPort = 80;

}  // namespace pixelos::hardware
