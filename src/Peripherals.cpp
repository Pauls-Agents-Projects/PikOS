#include "Peripherals.h"

#include <Wire.h>
#include <sys/time.h>
#include <time.h>

#include <algorithm>
#include <math.h>

#include "Hardware.h"

namespace pixelos {
namespace {

constexpr uint32_t kDebounceMs = 28;
constexpr uint32_t kLongPressMs = 1200;
constexpr uint8_t kChargingRiseWindows = 3;
constexpr uint8_t kChargingFallWindows = 2;
constexpr uint8_t kBuzzerChannel = 6;
constexpr uint8_t kBuzzerResolutionBits = 10;

uint8_t bcdToDecimal(uint8_t value) {
  return (value >> 4) * 10 + (value & 0x0f);
}

uint8_t decimalToBcd(uint8_t value) {
  return ((value / 10) << 4) | (value % 10);
}

bool timeIsValid(time_t value) {
  return value > 1672531200;  // 2023-01-01 UTC
}

bool toneIsDisabled(const String& encoded, const String& toneName) {
  if (encoded.isEmpty() || toneName.isEmpty() || toneName == "off") {
    return false;
  }
  return ("," + encoded + ",").indexOf("," + toneName + ",") >= 0;
}

float batteryStateOfCharge(uint16_t raw, const AppConfig& config) {
  const float normalized =
      constrain((static_cast<float>(raw) - config.batteryEmptyRaw) /
                    max<float>(30.0f, config.batteryFullRaw -
                                          config.batteryEmptyRaw),
                0.0f, 1.0f);
  // A lithium cell spends much more of its useful runtime in the middle of
  // its voltage curve than a linear ADC map suggests. Interpolating these
  // normalized anchors gives a steadier and more truthful remaining-runtime
  // estimate while retaining per-device empty/full calibration.
  static constexpr float kVoltage[] = {
      0.00f, 0.06f, 0.16f, 0.30f, 0.45f, 0.60f,
      0.74f, 0.86f, 0.94f, 0.98f, 1.00f};
  static constexpr float kPercent[] = {
      0.0f, 3.0f, 8.0f, 18.0f, 35.0f, 55.0f,
      75.0f, 88.0f, 96.0f, 100.0f, 100.0f};
  for (size_t index = 1; index < sizeof(kVoltage) / sizeof(kVoltage[0]);
       ++index) {
    if (normalized <= kVoltage[index]) {
      const float position =
          (normalized - kVoltage[index - 1]) /
          (kVoltage[index] - kVoltage[index - 1]);
      return kPercent[index - 1] +
             position * (kPercent[index] - kPercent[index - 1]);
    }
  }
  return 100.0f;
}

time_t utcEpoch(int year, unsigned month, unsigned day, unsigned hour,
                unsigned minute, unsigned second) {
  // Howard Hinnant's civil-date conversion, with 1970-01-01 as epoch.
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const int64_t days = era * 146097LL + dayOfEra - 719468LL;
  return static_cast<time_t>(
      days * 86400LL + hour * 3600ULL + minute * 60ULL + second);
}

}  // namespace

void Peripherals::begin(const AppConfig& config) {
  analogReadResolution(10);
  analogSetPinAttenuation(hardware::kBatteryPin, ADC_11db);
  analogSetPinAttenuation(hardware::kLightPin, ADC_11db);
  pinMode(hardware::kBatteryPin, INPUT);
  pinMode(hardware::kLightPin, INPUT);
  // The TC001 divider/ADC node rises for several hundred milliseconds after
  // reset. Sampling immediately makes a charged cell appear ~5% lower until
  // the running filter catches up.
  delay(800);
  for (uint8_t index = 0; index < 9; ++index) {
    batterySamples_[index] = analogRead(hardware::kBatteryPin);
    delay(25);
  }
  std::sort(batterySamples_, batterySamples_ + 9);
  smoothedBattery_ = batterySamples_[4];
  readings_.batteryRaw =
      static_cast<uint16_t>(lroundf(smoothedBattery_));
  smoothedBatteryPercent_ =
      batteryStateOfCharge(readings_.batteryRaw, config);
  readings_.batteryPercent = static_cast<uint8_t>(
      constrain(lroundf(smoothedBatteryPercent_), 0L, 100L));
  if (batteryPreferences_.begin("pikos-battery", false)) {
    readings_.batteryObservedMin =
        batteryPreferences_.getUShort("observed_min", readings_.batteryRaw);
    readings_.batteryObservedMax =
        batteryPreferences_.getUShort("observed_max", readings_.batteryRaw);
    readings_.batteryProfileSamples =
        batteryPreferences_.getULong("samples", 0);
  } else {
    readings_.batteryObservedMin = readings_.batteryRaw;
    readings_.batteryObservedMax = readings_.batteryRaw;
  }
  batterySampleCount_ = 9;
  batterySampleIndex_ = 0;
  recordBatteryHistory(millis());

  buttonQueue_ = xQueueCreateStatic(
      kButtonQueueDepth, sizeof(ButtonEvent), buttonQueueStorage_,
      &buttonQueueBuffer_);
  buttons_[0].pin = hardware::kButtonLeftPin;
  buttons_[1].pin = hardware::kButtonMiddlePin;
  buttons_[2].pin = hardware::kButtonRightPin;
  for (ButtonState& button : buttons_) {
    pinMode(button.pin, INPUT_PULLUP);
    button.rawPressed = digitalRead(button.pin) == LOW;
    button.stablePressed = button.rawPressed;
  }

  Wire.begin(hardware::kI2cSdaPin, hardware::kI2cSclPin);
  Wire.setClock(100000);
  lastClimateMs_ = millis();
  startSht3xRead(lastClimateMs_);
  portENTER_CRITICAL(&readingsMux_);
  publishedReadings_ = readings_;
  portEXIT_CRITICAL(&readingsMux_);
}

void Peripherals::pollButtons(uint32_t nowMs) {
  if (lastButtonPollMs_ != 0) {
    const uint32_t gap = nowMs - lastButtonPollMs_;
    portENTER_CRITICAL(&buttonTelemetryMux_);
    buttonTelemetry_.lastPollGapMs = gap;
    buttonTelemetry_.maximumPollGapMs =
        max(buttonTelemetry_.maximumPollGapMs, gap);
    portEXIT_CRITICAL(&buttonTelemetryMux_);
  }
  lastButtonPollMs_ = nowMs;
  updateButton(buttons_[0], ButtonId::Left, nowMs);
  updateButton(buttons_[1], ButtonId::Middle, nowMs);
  updateButton(buttons_[2], ButtonId::Right, nowMs);
}

void Peripherals::tick(uint32_t nowMs, const AppConfig& config) {
  if (nowMs - lastAnalogMs_ >= 100) {
    lastAnalogMs_ = nowMs;
    const uint16_t light = analogRead(hardware::kLightPin);
    if (smoothedLight_ == 0.0f) {
      smoothedLight_ = light;
    } else {
      smoothedLight_ = smoothedLight_ * 0.88f + light * 0.12f;
    }
    readings_.lightRaw = static_cast<uint16_t>(smoothedLight_);
  }

  // Battery voltage changes slowly. One sample per second, a 9-sample median,
  // and a slow EWMA reject Wi-Fi/LED current spikes that made the old 10 Hz
  // single-pole reading jump around.
  if (lastBatteryMs_ == 0 || nowMs - lastBatteryMs_ >= 1000) {
    lastBatteryMs_ = nowMs;
    const uint16_t battery = analogRead(hardware::kBatteryPin);
    if (battery > 100 && battery < 1000) {
      batterySamples_[batterySampleIndex_] = battery;
      batterySampleIndex_ = (batterySampleIndex_ + 1) % 9;
      batterySampleCount_ = min<uint8_t>(9, batterySampleCount_ + 1);
      uint16_t sorted[9]{};
      std::copy(batterySamples_, batterySamples_ + batterySampleCount_,
                sorted);
      std::sort(sorted, sorted + batterySampleCount_);
      const uint16_t median = sorted[batterySampleCount_ / 2];
      smoothedBattery_ =
          smoothedBattery_ == 0.0f
              ? median
              : smoothedBattery_ * 0.85f + median * 0.15f;
      readings_.batteryRaw =
          static_cast<uint16_t>(lroundf(smoothedBattery_));
      const float estimated =
          batteryStateOfCharge(readings_.batteryRaw, config);
      smoothedBatteryPercent_ =
          smoothedBatteryPercent_ < 0.0f
              ? estimated
              : smoothedBatteryPercent_ * 0.82f + estimated * 0.18f;
      readings_.batteryPercent = static_cast<uint8_t>(
          constrain(lroundf(smoothedBatteryPercent_), 0L, 100L));
      recordBatteryHistory(nowMs);
      ++readings_.batteryProfileSamples;
      if (readings_.batteryObservedMin == 0 ||
          readings_.batteryRaw < readings_.batteryObservedMin) {
        readings_.batteryObservedMin = readings_.batteryRaw;
        batteryProfileDirty_ = true;
      }
      if (readings_.batteryRaw > readings_.batteryObservedMax) {
        readings_.batteryObservedMax = readings_.batteryRaw;
        batteryProfileDirty_ = true;
      }

      if (batteryTrendReferenceMs_ == 0) {
        batteryTrendReferenceMs_ = nowMs;
        batteryTrendReference_ = readings_.batteryRaw;
      } else if (nowMs - batteryTrendReferenceMs_ >= 60000) {
        const int difference =
            static_cast<int>(readings_.batteryRaw) - batteryTrendReference_;
        readings_.batteryTrend =
            difference >= 3 ? 1 : (difference <= -3 ? -1 : 0);
        if (readings_.batteryTrend > 0) {
          consecutiveBatteryRiseWindows_ = min<uint8_t>(
              kChargingRiseWindows, consecutiveBatteryRiseWindows_ + 1);
          consecutiveBatteryFallWindows_ = 0;
          if (consecutiveBatteryRiseWindows_ >= kChargingRiseWindows) {
            readings_.batteryChargingLikely = true;
          }
        } else if (readings_.batteryTrend < 0) {
          consecutiveBatteryFallWindows_ = min<uint8_t>(
              kChargingFallWindows, consecutiveBatteryFallWindows_ + 1);
          consecutiveBatteryRiseWindows_ = 0;
          if (consecutiveBatteryFallWindows_ >= kChargingFallWindows) {
            readings_.batteryChargingLikely = false;
          }
        } else {
          // A one-off load reduction can raise cell voltage. It must not
          // accumulate into a charging claim unless the rise persists across
          // consecutive minute windows. Once charging is likely, stable
          // voltage near full charge intentionally retains that weak hint.
          consecutiveBatteryRiseWindows_ = 0;
          consecutiveBatteryFallWindows_ = 0;
        }
        batteryTrendReferenceMs_ = nowMs;
        batteryTrendReference_ = readings_.batteryRaw;
      }
      if (lastBatteryProfileLogMs_ == 0 ||
          nowMs - lastBatteryProfileLogMs_ >= 300000UL) {
        lastBatteryProfileLogMs_ = nowMs;
        Serial.printf("[battery] raw=%u estimate=%u%% trend=%d likely-charging=%s observed=%u..%u samples=%lu\n",
                      readings_.batteryRaw, readings_.batteryPercent,
                      readings_.batteryTrend,
                      readings_.batteryChargingLikely ? "yes" : "no",
                      readings_.batteryObservedMin,
                      readings_.batteryObservedMax,
                      static_cast<unsigned long>(readings_.batteryProfileSamples));
      }
      // Persist only changed extrema and batch writes at half-hour intervals;
      // this builds a useful discharge profile without wearing out NVS.
      if (batteryProfileDirty_ &&
          (lastBatteryProfileSaveMs_ == 0 ||
           nowMs - lastBatteryProfileSaveMs_ >= 1800000UL)) {
        batteryPreferences_.putUShort("observed_min",
                                      readings_.batteryObservedMin);
        batteryPreferences_.putUShort("observed_max",
                                      readings_.batteryObservedMax);
        batteryPreferences_.putULong("samples",
                                     readings_.batteryProfileSamples);
        lastBatteryProfileSaveMs_ = nowMs;
        batteryProfileDirty_ = false;
      }
    }
  }

  if (climateReadPending_ &&
      static_cast<int32_t>(nowMs - climateReadReadyMs_) >= 0) {
    finishSht3xRead();
  }
  if (!climateReadPending_ && nowMs - lastClimateMs_ >= 10000) {
    lastClimateMs_ = nowMs;
    startSht3xRead(nowMs);
  }
  // Publish one coherent sensor generation to the render task. All mutable
  // sampling/filter state stays on the main task; readers never race a
  // battery or climate update midway through the struct copy.
  if (lastReadingsPublishMs_ == 0 ||
      nowMs - lastReadingsPublishMs_ >= 50) {
    lastReadingsPublishMs_ = nowMs;
    portENTER_CRITICAL(&readingsMux_);
    publishedReadings_ = readings_;
    portEXIT_CRITICAL(&readingsMux_);
  }
}

void Peripherals::updateButton(ButtonState& button, ButtonId id,
                               uint32_t nowMs) {
  const bool pressed = digitalRead(button.pin) == LOW;
  if (pressed != button.rawPressed) {
    button.rawPressed = pressed;
    button.rawChangedMs = nowMs;
  }

  if (button.stablePressed != button.rawPressed &&
      nowMs - button.rawChangedMs >= kDebounceMs) {
    button.stablePressed = button.rawPressed;
    if (button.stablePressed) {
      button.pressedEdgeMs = button.rawChangedMs;
      button.pressedMs = nowMs;
      button.longSent = false;
      queueButtonEvent(id, ButtonEventType::Pressed,
                       button.pressedEdgeMs, nowMs);
    }
  }

  if (button.stablePressed && !button.longSent &&
      nowMs - button.pressedMs >= kLongPressMs) {
    button.longSent = true;
    queueButtonEvent(id, ButtonEventType::LongPressed,
                     button.pressedEdgeMs, nowMs);
  }
}

void Peripherals::queueButtonEvent(ButtonId id, ButtonEventType type,
                                   uint32_t rawEdgeAtMs,
                                   uint32_t nowMs) {
  ButtonEvent event;
  event.button = id;
  event.type = type;
  event.rawEdgeAtMs = rawEdgeAtMs;
  event.queuedAtMs = nowMs;
  const bool queued =
      buttonQueue_ != nullptr && xQueueSend(buttonQueue_, &event, 0) == pdTRUE;
  portENTER_CRITICAL(&buttonTelemetryMux_);
  ++buttonTelemetry_.detectedEvents;
  if (queued) {
    ++buttonTelemetry_.queuedEvents;
  } else {
    ++buttonTelemetry_.droppedEvents;
  }
  portEXIT_CRITICAL(&buttonTelemetryMux_);
}

SensorReadings Peripherals::readings() const {
  portENTER_CRITICAL(&readingsMux_);
  const SensorReadings copy = publishedReadings_;
  portEXIT_CRITICAL(&readingsMux_);
  return copy;
}

void Peripherals::recordBatteryHistory(uint32_t nowMs) {
  constexpr uint32_t kHistoryIntervalMs = 300000UL;
  if (lastBatteryHistoryMs_ != 0 &&
      nowMs - lastBatteryHistoryMs_ < kHistoryIntervalMs) {
    return;
  }
  lastBatteryHistoryMs_ = nowMs;
  portENTER_CRITICAL(&batteryHistoryMux_);
  batteryHistoryPercent_[batteryHistoryNext_] = readings_.batteryPercent;
  batteryHistoryRaw_[batteryHistoryNext_] = readings_.batteryRaw;
  batteryHistoryAtMs_[batteryHistoryNext_] = nowMs;
  batteryHistoryNext_ = static_cast<uint8_t>(
      (batteryHistoryNext_ + 1) % BatteryHistorySnapshot::kMaximumSamples);
  batteryHistoryCount_ = min<uint8_t>(
      BatteryHistorySnapshot::kMaximumSamples, batteryHistoryCount_ + 1);
  portEXIT_CRITICAL(&batteryHistoryMux_);
}

BatteryHistorySnapshot Peripherals::batteryHistory() const {
  BatteryHistorySnapshot snapshot;
  uint32_t times[BatteryHistorySnapshot::kMaximumSamples]{};
  const uint32_t nowMs = millis();
  portENTER_CRITICAL(&batteryHistoryMux_);
  snapshot.sampleCount = batteryHistoryCount_;
  const uint8_t oldest =
      batteryHistoryCount_ == BatteryHistorySnapshot::kMaximumSamples
          ? batteryHistoryNext_
          : 0;
  for (uint8_t index = 0; index < batteryHistoryCount_; ++index) {
    const uint8_t source = static_cast<uint8_t>(
        (oldest + index) % BatteryHistorySnapshot::kMaximumSamples);
    snapshot.percent[index] = batteryHistoryPercent_[source];
    snapshot.raw[index] = batteryHistoryRaw_[source];
    times[index] = batteryHistoryAtMs_[source];
  }
  portEXIT_CRITICAL(&batteryHistoryMux_);

  if (snapshot.sampleCount == 0) return snapshot;
  const uint8_t newest = snapshot.sampleCount - 1;
  for (uint8_t index = 0; index < snapshot.sampleCount; ++index) {
    snapshot.ageSeconds[index] = (nowMs - times[index]) / 1000UL;
  }
  snapshot.newestAgeSeconds = snapshot.ageSeconds[newest];
  const uint32_t elapsedMs = times[newest] - times[0];
  // Ten minutes is enough to show direction without presenting a single ADC
  // fluctuation as a trustworthy battery-runtime estimate.
  if (snapshot.sampleCount >= 3 && elapsedMs >= 600000UL) {
    snapshot.drainPercentPerHour =
        (static_cast<float>(snapshot.percent[0]) -
         static_cast<float>(snapshot.percent[newest])) *
        3600000.0f / static_cast<float>(elapsedMs);
    if (snapshot.drainPercentPerHour > 0.1f) {
      snapshot.estimatedHoursRemaining =
          snapshot.percent[newest] / snapshot.drainPercentPerHour;
    }
  }
  return snapshot;
}

bool Peripherals::popButtonEvent(ButtonEvent& event) {
  return buttonQueue_ != nullptr &&
         xQueueReceive(buttonQueue_, &event, 0) == pdTRUE;
}

void Peripherals::recordButtonApplied(const ButtonEvent& event,
                                      uint32_t appliedAtMs) {
  // A short press includes GPIO polling and debounce. A long press uses its
  // enqueue time so the intentional 1.2-second hold is not misreported as a
  // scheduler stall.
  const uint32_t reference =
      event.type == ButtonEventType::Pressed && event.rawEdgeAtMs != 0
          ? event.rawEdgeAtMs
          : event.queuedAtMs;
  const uint32_t latency = appliedAtMs - reference;
  portENTER_CRITICAL(&buttonTelemetryMux_);
  ++buttonTelemetry_.appliedEvents;
  buttonTelemetry_.lastLatencyMs = latency;
  buttonTelemetry_.maximumLatencyMs =
      max(buttonTelemetry_.maximumLatencyMs, latency);
  const uint8_t bucket = min<uint32_t>(20, latency / 10);
  if (buttonLatencyHistogram_[bucket] != UINT16_MAX) {
    ++buttonLatencyHistogram_[bucket];
    ++buttonLatencySamples_;
  }
  portEXIT_CRITICAL(&buttonTelemetryMux_);
}

ButtonTelemetry Peripherals::buttonTelemetry() const {
  portENTER_CRITICAL(&buttonTelemetryMux_);
  ButtonTelemetry copy = buttonTelemetry_;
  if (buttonLatencySamples_ != 0) {
    const uint32_t target =
        (buttonLatencySamples_ * 95U + 99U) / 100U;
    uint32_t cumulative = 0;
    for (uint8_t bucket = 0; bucket < 21; ++bucket) {
      cumulative += buttonLatencyHistogram_[bucket];
      if (cumulative >= target) {
        copy.p95LatencyMs = bucket == 20 ? 200 : (bucket + 1U) * 10U;
        break;
      }
    }
  }
  portEXIT_CRITICAL(&buttonTelemetryMux_);
  return copy;
}

void Peripherals::resetButtonTelemetry() {
  portENTER_CRITICAL(&buttonTelemetryMux_);
  buttonTelemetry_ = ButtonTelemetry{};
  memset(buttonLatencyHistogram_, 0, sizeof(buttonLatencyHistogram_));
  buttonLatencySamples_ = 0;
  portEXIT_CRITICAL(&buttonTelemetryMux_);
}

uint8_t Peripherals::desiredBrightness(const AppConfig& config) const {
  const SensorReadings current = readings();
  uint8_t desired = config.manualBrightness;
  if (!config.autoBrightness) {
    desired = config.manualBrightness;
  } else {
    const float normalized =
        constrain((static_cast<float>(current.lightRaw) - config.ldrDark) /
                      static_cast<float>(config.ldrBright - config.ldrDark),
                  0.0f, 1.0f);
    const float curved = powf(normalized, config.brightnessGamma);
    desired = static_cast<uint8_t>(
        config.minimumBrightness +
        curved * (config.maximumBrightness - config.minimumBrightness));
  }
  if (config.batterySaverEnabled && current.batteryRaw > 0 &&
      !current.batteryChargingLikely &&
      current.batteryPercent <= config.batterySaverThreshold) {
    desired = min<uint8_t>(desired, config.batterySaverBrightness);
  }
  return desired;
}

bool Peripherals::batterySaverActive(const AppConfig& config) const {
  const SensorReadings current = readings();
  return config.batterySaverEnabled && current.batteryRaw > 0 &&
         !current.batteryChargingLikely &&
         current.batteryPercent <= config.batterySaverThreshold;
}

uint8_t Peripherals::crc8(uint8_t high, uint8_t low) {
  uint8_t crc = 0xff;
  const uint8_t values[2] = {high, low};
  for (uint8_t value : values) {
    crc ^= value;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x31)
                         : static_cast<uint8_t>(crc << 1);
    }
  }
  return crc;
}

bool Peripherals::startSht3xRead(uint32_t nowMs) {
  Wire.beginTransmission(hardware::kSht3xAddress);
  Wire.write(0x24);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) {
    readings_.climateAvailable = false;
    climateReadPending_ = false;
    return false;
  }
  // High-repeatability, no-clock-stretch mode needs at most about 15 ms.
  // Return to the main loop instead of blocking button and web service.
  climateReadPending_ = true;
  climateReadReadyMs_ = nowMs + 20;
  return true;
}

bool Peripherals::finishSht3xRead() {
  climateReadPending_ = false;
  if (Wire.requestFrom(hardware::kSht3xAddress, static_cast<uint8_t>(6)) != 6) {
    readings_.climateAvailable = false;
    return false;
  }
  const uint8_t tHigh = Wire.read();
  const uint8_t tLow = Wire.read();
  const uint8_t tCrc = Wire.read();
  const uint8_t hHigh = Wire.read();
  const uint8_t hLow = Wire.read();
  const uint8_t hCrc = Wire.read();
  if (crc8(tHigh, tLow) != tCrc || crc8(hHigh, hLow) != hCrc) {
    readings_.climateAvailable = false;
    return false;
  }
  const uint16_t rawTemperature =
      (static_cast<uint16_t>(tHigh) << 8) | tLow;
  const uint16_t rawHumidity = (static_cast<uint16_t>(hHigh) << 8) | hLow;
  readings_.temperatureC =
      -45.0f + 175.0f * static_cast<float>(rawTemperature) / 65535.0f;
  readings_.humidityPercent =
      100.0f * static_cast<float>(rawHumidity) / 65535.0f;
  readings_.climateAvailable = true;
  return true;
}

bool Peripherals::restoreClockFromRtc() {
  Wire.beginTransmission(hardware::kRtcAddress);
  Wire.write(0x00);
  if (Wire.endTransmission(false) != 0 ||
      Wire.requestFrom(hardware::kRtcAddress, static_cast<uint8_t>(7)) != 7) {
    return false;
  }

  const uint8_t second = bcdToDecimal(Wire.read() & 0x7f);
  const uint8_t minute = bcdToDecimal(Wire.read() & 0x7f);
  const uint8_t hourRegister = Wire.read();
  uint8_t hour = 0;
  if (hourRegister & 0x40) {
    hour = bcdToDecimal(hourRegister & 0x1f);
    if (hourRegister & 0x20) {
      hour = (hour % 12) + 12;
    }
  } else {
    hour = bcdToDecimal(hourRegister & 0x3f);
  }
  Wire.read();  // day of week
  const uint8_t day = bcdToDecimal(Wire.read() & 0x3f);
  const uint8_t monthRegister = Wire.read();
  const uint8_t month = bcdToDecimal(monthRegister & 0x1f);
  const int year =
      2000 + bcdToDecimal(Wire.read()) + ((monthRegister & 0x80) ? 100 : 0);

  const time_t epoch = utcEpoch(year, month, day, hour, minute, second);
  if (!timeIsValid(epoch)) {
    return false;
  }
  timeval tv{epoch, 0};
  settimeofday(&tv, nullptr);
  return true;
}

bool Peripherals::writeSystemClockToRtc() {
  const time_t now = time(nullptr);
  if (!timeIsValid(now)) {
    return false;
  }
  tm utc{};
  gmtime_r(&now, &utc);

  Wire.beginTransmission(hardware::kRtcAddress);
  Wire.write(0x00);
  Wire.write(decimalToBcd(utc.tm_sec));
  Wire.write(decimalToBcd(utc.tm_min));
  Wire.write(decimalToBcd(utc.tm_hour));
  Wire.write(decimalToBcd(utc.tm_wday == 0 ? 7 : utc.tm_wday));
  Wire.write(decimalToBcd(utc.tm_mday));
  Wire.write(decimalToBcd(utc.tm_mon + 1));
  Wire.write(decimalToBcd((utc.tm_year + 1900) - 2000));
  return Wire.endTransmission() == 0;
}

void Buzzer::begin() {
  ledcSetup(kBuzzerChannel, 2000, kBuzzerResolutionBits);
  ledcAttachPin(hardware::kBuzzerPin, kBuzzerChannel);
  ledcWrite(kBuzzerChannel, 0);
}

void Buzzer::configure(uint8_t volumePercent, uint8_t pitchPercent,
                       uint16_t highNoteBoostPercent) {
  volumePercent_ = min<uint8_t>(volumePercent, 100);
  pitchPercent_ =
      min<uint8_t>(max<uint8_t>(pitchPercent, 80), 120);
  highNoteBoostPercent_ =
      min<uint16_t>(max<uint16_t>(highNoteBoostPercent, 100), 300);
}

void Buzzer::startMelody(const ToneStep* steps, uint8_t count) {
  melodyCount_ = min<uint8_t>(count, 96);
  melodyIndex_ = 0;
  for (uint8_t index = 0; index < melodyCount_; ++index) {
    melody_[index] = steps[index];
  }
  nextToneMs_ = millis();
}

void Buzzer::playConfigured(const String& toneName,
                            const AppConfig& config) {
  configure(config.buzzerVolume, config.buzzerPitchPercent,
            config.buzzerHighNoteBoostPercent);
  if (toneIsDisabled(config.disabledBuiltinTones, toneName)) {
    play("off");
    return;
  }
  if (toneName == "custom1") {
    if (!playRtttl(config.customRtttl1)) {
      play("off");
    }
    return;
  }
  if (toneName == "custom2") {
    if (!playRtttl(config.customRtttl2)) {
      play("off");
    }
    return;
  }
  if (toneName == "custom3") {
    if (!playRtttl(config.customRtttl3)) {
      play("off");
    }
    return;
  }
  if (toneName == "custom4") {
    if (!playRtttl(config.customRtttl4)) {
      play("off");
    }
    return;
  }
  if (toneName == "custom5") {
    if (!playRtttl(config.customRtttl5)) {
      play("off");
    }
    return;
  }
  if (toneName == "custom6") {
    if (!playRtttl(config.customRtttl6)) {
      play("off");
    }
    return;
  }
  play(toneName);
}

bool Buzzer::playRtttl(const String& input) {
  String rtttl = input;
  rtttl.trim();
  const int firstColon = rtttl.indexOf(':');
  const int secondColon =
      firstColon < 0 ? -1 : rtttl.indexOf(':', firstColon + 1);
  if (firstColon < 0 || secondColon < 0 ||
      secondColon + 1 >= static_cast<int>(rtttl.length())) {
    return false;
  }

  uint16_t defaultDuration = 4;
  uint8_t defaultOctave = 6;
  uint16_t bpm = 63;
  String settings = rtttl.substring(firstColon + 1, secondColon);
  settings.toLowerCase();
  int settingStart = 0;
  while (settingStart <= static_cast<int>(settings.length())) {
    int settingEnd = settings.indexOf(',', settingStart);
    if (settingEnd < 0) {
      settingEnd = settings.length();
    }
    String setting = settings.substring(settingStart, settingEnd);
    setting.trim();
    if (setting.startsWith("d=")) {
      defaultDuration =
          constrain(setting.substring(2).toInt(), 1L, 64L);
    } else if (setting.startsWith("o=")) {
      defaultOctave =
          static_cast<uint8_t>(
              constrain(setting.substring(2).toInt(), 3L, 8L));
    } else if (setting.startsWith("b=")) {
      bpm = constrain(setting.substring(2).toInt(), 25L, 900L);
    }
    if (settingEnd >= static_cast<int>(settings.length())) {
      break;
    }
    settingStart = settingEnd + 1;
  }

  ToneStep parsed[96]{};
  uint8_t count = 0;
  String notes = rtttl.substring(secondColon + 1);
  notes.toLowerCase();
  int noteStart = 0;
  const uint32_t wholeNoteMs = 240000UL / bpm;
  while (noteStart <= static_cast<int>(notes.length()) && count < 96) {
    int noteEnd = notes.indexOf(',', noteStart);
    if (noteEnd < 0) {
      noteEnd = notes.length();
    }
    String token = notes.substring(noteStart, noteEnd);
    token.trim();
    if (!token.isEmpty()) {
      size_t cursor = 0;
      uint16_t duration = 0;
      while (cursor < token.length() && isDigit(token[cursor])) {
        duration = duration * 10 + (token[cursor++] - '0');
      }
      if (duration == 0) {
        duration = defaultDuration;
      }
      if (cursor >= token.length()) {
        return false;
      }

      const char note = token[cursor++];
      if (note != 'a' && note != 'b' && note != 'c' && note != 'd' &&
          note != 'e' && note != 'f' && note != 'g' && note != 'p') {
        return false;
      }
      bool sharp = false;
      bool dotted = false;
      if (cursor < token.length() && token[cursor] == '#') {
        sharp = true;
        ++cursor;
      }
      if (cursor < token.length() && token[cursor] == '.') {
        dotted = true;
        ++cursor;
      }
      uint8_t octave = defaultOctave;
      if (cursor < token.length() && isDigit(token[cursor])) {
        octave = static_cast<uint8_t>(
            constrain(token[cursor++] - '0', 3, 8));
      }
      if (cursor < token.length() && token[cursor] == '.') {
        dotted = true;
      }

      uint32_t totalMs = wholeNoteMs / max<uint16_t>(duration, 1);
      if (dotted) {
        totalMs += totalMs / 2;
      }
      totalMs = constrain(totalMs, 12UL, 60000UL);

      uint16_t frequency = 0;
      if (note != 'p') {
        int semitone = 0;
        switch (note) {
          case 'c': semitone = 0; break;
          case 'd': semitone = 2; break;
          case 'e': semitone = 4; break;
          case 'f': semitone = 5; break;
          case 'g': semitone = 7; break;
          case 'a': semitone = 9; break;
          case 'b': semitone = 11; break;
          default: break;
        }
        semitone += sharp ? 1 : 0;
        const int midi = 12 * (static_cast<int>(octave) + 1) + semitone;
        frequency = static_cast<uint16_t>(
            constrain(lroundf(440.0f * powf(2.0f, (midi - 69) / 12.0f)),
                      31L, 12000L));
      }

      if (frequency == 0) {
        parsed[count++] = {
            0, static_cast<uint16_t>(min<uint32_t>(totalMs, 65535)), 0};
      } else {
        const uint16_t sounding =
            static_cast<uint16_t>(max<uint32_t>(10, totalMs * 9 / 10));
        parsed[count++] = {
            frequency, sounding,
            static_cast<uint16_t>(min<uint32_t>(totalMs - sounding, 65535))};
      }
    }
    if (noteEnd >= static_cast<int>(notes.length())) {
      break;
    }
    noteStart = noteEnd + 1;
  }

  if (count == 0) {
    return false;
  }
  startMelody(parsed, count);
  return true;
}

void Buzzer::play(const String& toneName) {
  if (toneName == "off") {
    melodyCount_ = 0;
    ledcWriteTone(kBuzzerChannel, 0);
    ledcWrite(kBuzzerChannel, 0);
    toneActive_ = false;
    return;
  }
  if (toneName == "click") {
    const ToneStep melody[] = {{2637, 70, 0}};
    startMelody(melody, 1);
    return;
  }
  if (toneName == "double") {
    const ToneStep melody[] = {{1760, 80, 45}, {2349, 110, 0}};
    startMelody(melody, 2);
    return;
  }
  if (toneName == "rise") {
    const ToneStep melody[] = {
        {1047, 70, 25}, {1319, 70, 25}, {1568, 70, 25}, {2093, 140, 0}};
    startMelody(melody, 4);
    return;
  }
  if (toneName == "boot") {
    const ToneStep melody[] = {
        {784, 65, 25}, {1047, 75, 25}, {1319, 85, 30},
        {1568, 105, 25}, {2093, 170, 0}};
    startMelody(melody, 5);
    return;
  }
  if (toneName == "soft") {
    const ToneStep melody[] = {
        {784, 110, 35}, {988, 110, 35}, {1175, 170, 0}};
    startMelody(melody, 3);
    return;
  }
  if (toneName == "alert") {
    const ToneStep melody[] = {
        {2093, 115, 60}, {0, 60, 10}, {2093, 115, 60},
        {0, 60, 10}, {2637, 180, 0}};
    startMelody(melody, 5);
    return;
  }
  if (toneName == "gran_vals") {
    // Monophonic phrase from Tárrega's public-domain Gran Vals, made famous
    // by classic Nokia phones. The TC001 passive buzzer can play one pitch
    // at a time, which is exactly what this melody requires.
    const ToneStep melody[] = {
        {1319, 110, 20}, {1175, 110, 20}, {740, 220, 35},
        {831, 220, 35},  {1109, 110, 20}, {988, 110, 20},
        {587, 220, 35},  {659, 220, 35},  {988, 110, 20},
        {880, 110, 20},  {554, 220, 35},  {659, 220, 35},
        {880, 360, 0}};
    startMelody(melody, 13);
    return;
  }
  if (toneName == "hourly") {
    const ToneStep melody[] = {
        {1047, 80, 30}, {1319, 80, 30},
        {1568, 80, 30}, {2093, 160, 0}};
    startMelody(melody, 4);
    return;
  }

  const ToneStep melody[] = {
      {1568, 85, 45}, {1976, 85, 55}, {2349, 135, 0}};
  startMelody(melody, 3);
}

void Buzzer::news() {
  play("news");
}

void Buzzer::hourly() {
  play("hourly");
}

void Buzzer::notification() {
  play("double");
}

void Buzzer::confirm() {
  play("click");
}

void Buzzer::tick(uint32_t nowMs) {
  if (toneActive_ &&
      static_cast<int32_t>(nowMs - toneOffMs_) >= 0) {
    ledcWrite(kBuzzerChannel, 0);
    toneActive_ = false;
  }
  if (melodyIndex_ >= melodyCount_ ||
      static_cast<int32_t>(nowMs - nextToneMs_) < 0) {
    return;
  }
  const ToneStep& step = melody_[melodyIndex_++];
  if (step.frequency == 0 || volumePercent_ == 0) {
    ledcWrite(kBuzzerChannel, 0);
    toneActive_ = false;
  } else {
    const uint32_t calibrated =
        static_cast<uint32_t>(step.frequency) * pitchPercent_ / 100U;
    ledcWriteTone(kBuzzerChannel, calibrated);
    // A passive piezo is very efficient. The old linear volume curve reached
    // 50% duty before frequency compensation, so louder settings clipped the
    // high notes while quiet settings did not. That changed the balance (and
    // apparent melody) as the volume slider moved.
    const uint32_t maximumDuty =
        (1U << (kBuzzerResolutionBits - 1));
    // At very low duty cycles, short high-frequency pulses barely excite the
    // TC001 piezo. Gradually boost only notes above 1.4 kHz so a quiet 5%
    // setting stays balanced without making the whole melody louder.
    const uint32_t highRange =
        calibrated > 1400 ? min<uint32_t>(calibrated - 1400, 5000) : 0;
    const uint32_t gainPercent =
        100U + (highNoteBoostPercent_ - 100U) * highRange / 5000U;
    // Reserve enough PWM headroom for the configured high-note gain, then use
    // a perceptual curve anchored so the user's preferred 5% setting remains
    // close to its former level. Every volume now has the same note-to-note
    // gain ratio and therefore the same timbre; only loudness changes.
    const uint32_t baseCeiling =
        min<uint32_t>(205U,
                      maximumDuty * 100U / highNoteBoostPercent_);
    const float loudness =
        powf(static_cast<float>(volumePercent_) / 100.0f, 0.70f);
    const uint32_t baseDuty =
        static_cast<uint32_t>(lroundf(baseCeiling * loudness));
    // Microphone calibration on the TC001's piezo showed a strong nonlinear
    // crossover: above roughly C6, notes were 10 dB quieter at 5%, but 5 dB
    // louder at 100%. Correct that crossover as part of the volume curve so
    // changing only the slider no longer changes the apparent melody.
    uint32_t upperGainPercent = 100;
    const uint32_t volume = volumePercent_;
    if (volume <= 5) {
      upperGainPercent = 150;
    } else if (volume <= 10) {
      upperGainPercent = 150 - (volume - 5) * 15 / 5;
    } else if (volume <= 25) {
      upperGainPercent = 135 + (volume - 10) * 20 / 15;
    } else if (volume <= 50) {
      upperGainPercent = 155 - (volume - 25) * 70 / 25;
    } else {
      upperGainPercent = 85 - (volume - 50) * 28 / 50;
    }
    const uint32_t upperBlend =
        calibrated <= 650
            ? 0
            : calibrated >= 1050
                  ? 100
                  : (calibrated - 650) * 100 / 400;
    const int32_t volumeEqPercent =
        100 + (static_cast<int32_t>(upperGainPercent) - 100) *
                  static_cast<int32_t>(upperBlend) / 100;
    const uint32_t compensatedDuty =
        baseDuty * gainPercent * static_cast<uint32_t>(volumeEqPercent) /
        10000U;
    ledcWrite(kBuzzerChannel, min<uint32_t>(maximumDuty, compensatedDuty));
    toneOffMs_ = nowMs + step.durationMs;
    toneActive_ = true;
  }
  nextToneMs_ = nowMs + step.durationMs + step.gapMs;
}

}  // namespace pixelos
