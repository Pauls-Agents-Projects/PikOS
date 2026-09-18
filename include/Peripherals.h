#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "Config.h"

namespace pixelos {

struct SensorReadings {
  bool climateAvailable = false;
  float temperatureC = NAN;
  float humidityPercent = NAN;
  uint16_t lightRaw = 0;
  uint16_t batteryRaw = 0;
  uint8_t batteryPercent = 0;
  int8_t batteryTrend = 0;
  // The TC001 does not expose USB VBUS or its charger's status pin. This is
  // deliberately only a sustained-voltage-rise hint, never a definitive
  // external-power reading.
  bool batteryChargingLikely = false;
  uint16_t batteryObservedMin = 0;
  uint16_t batteryObservedMax = 0;
  uint32_t batteryProfileSamples = 0;
};

enum class ButtonId : uint8_t { Left, Middle, Right };
enum class ButtonEventType : uint8_t { Pressed, LongPressed };

struct ButtonEvent {
  ButtonId button;
  ButtonEventType type;
  // First observation of the electrical edge. Keeping this separate from the
  // queue time makes debounce and scheduler stalls visible in telemetry.
  uint32_t rawEdgeAtMs = 0;
  uint32_t queuedAtMs = 0;
};

struct ButtonTelemetry {
  uint32_t detectedEvents = 0;
  uint32_t queuedEvents = 0;
  uint32_t appliedEvents = 0;
  uint32_t droppedEvents = 0;
  uint32_t lastPollGapMs = 0;
  uint32_t maximumPollGapMs = 0;
  uint32_t lastLatencyMs = 0;
  uint32_t p95LatencyMs = 0;
  uint32_t maximumLatencyMs = 0;
};

struct BatteryHistorySnapshot {
  // Eight hours at five-minute resolution covers a typical unplugged session
  // while remaining a small, RAM-only ring buffer (no flash wear).
  static constexpr uint8_t kMaximumSamples = 96;
  uint16_t intervalSeconds = 300;
  uint8_t sampleCount = 0;
  uint32_t newestAgeSeconds = 0;
  // Age of every sample at snapshot time. Unlike a synthetic fixed interval,
  // this preserves pauses caused by sleep, reboot, or a busy sensor task.
  uint32_t ageSeconds[kMaximumSamples]{};
  uint8_t percent[kMaximumSamples]{};
  uint16_t raw[kMaximumSamples]{};
  // Positive means discharge; negative means the observed percentage rose.
  // These are trend estimates, not electrical current measurements.
  float drainPercentPerHour = NAN;
  float estimatedHoursRemaining = NAN;
};

class Peripherals {
 public:
  void begin(const AppConfig& config);
  // Poll from the latency-critical display/input task. Sensor tick() is kept
  // separate so I2C, flash, or network work on the main task cannot hide a
  // physical edge.
  void pollButtons(uint32_t nowMs);
  void tick(uint32_t nowMs, const AppConfig& config);
  SensorReadings readings() const;
  BatteryHistorySnapshot batteryHistory() const;
  bool popButtonEvent(ButtonEvent& event);
  void recordButtonApplied(const ButtonEvent& event, uint32_t appliedAtMs);
  ButtonTelemetry buttonTelemetry() const;
  void resetButtonTelemetry();
  uint8_t desiredBrightness(const AppConfig& config) const;
  bool batterySaverActive(const AppConfig& config) const;
  bool restoreClockFromRtc();
  bool writeSystemClockToRtc();

 private:
  struct ButtonState {
    uint8_t pin = 0;
    bool rawPressed = false;
    bool stablePressed = false;
    bool longSent = false;
    uint32_t rawChangedMs = 0;
    uint32_t pressedEdgeMs = 0;
    uint32_t pressedMs = 0;
  };

  void updateButton(ButtonState& button, ButtonId id, uint32_t nowMs);
  void queueButtonEvent(ButtonId id, ButtonEventType type,
                        uint32_t rawEdgeAtMs, uint32_t nowMs);
  bool startSht3xRead(uint32_t nowMs);
  bool finishSht3xRead();
  void recordBatteryHistory(uint32_t nowMs);
  static uint8_t crc8(uint8_t high, uint8_t low);

  ButtonState buttons_[3];
  static constexpr UBaseType_t kButtonQueueDepth = 8;
  StaticQueue_t buttonQueueBuffer_{};
  uint8_t buttonQueueStorage_[kButtonQueueDepth * sizeof(ButtonEvent)]{};
  QueueHandle_t buttonQueue_ = nullptr;
  mutable portMUX_TYPE buttonTelemetryMux_ = portMUX_INITIALIZER_UNLOCKED;
  ButtonTelemetry buttonTelemetry_{};
  uint16_t buttonLatencyHistogram_[21]{};
  uint32_t buttonLatencySamples_ = 0;
  uint32_t lastButtonPollMs_ = 0;
  mutable portMUX_TYPE readingsMux_ = portMUX_INITIALIZER_UNLOCKED;
  SensorReadings readings_;
  SensorReadings publishedReadings_;
  uint32_t lastReadingsPublishMs_ = 0;
  uint32_t lastAnalogMs_ = 0;
  uint32_t lastBatteryMs_ = 0;
  uint32_t lastClimateMs_ = 0;
  float smoothedLight_ = 0.0f;
  float smoothedBattery_ = 0.0f;
  float smoothedBatteryPercent_ = -1.0f;
  uint16_t batterySamples_[9]{};
  uint8_t batterySampleCount_ = 0;
  uint8_t batterySampleIndex_ = 0;
  uint16_t batteryTrendReference_ = 0;
  uint32_t batteryTrendReferenceMs_ = 0;
  uint8_t consecutiveBatteryRiseWindows_ = 0;
  uint8_t consecutiveBatteryFallWindows_ = 0;
  Preferences batteryPreferences_;
  uint32_t lastBatteryProfileSaveMs_ = 0;
  uint32_t lastBatteryProfileLogMs_ = 0;
  bool batteryProfileDirty_ = false;
  mutable portMUX_TYPE batteryHistoryMux_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t batteryHistoryPercent_[BatteryHistorySnapshot::kMaximumSamples]{};
  uint16_t batteryHistoryRaw_[BatteryHistorySnapshot::kMaximumSamples]{};
  uint32_t batteryHistoryAtMs_[BatteryHistorySnapshot::kMaximumSamples]{};
  uint8_t batteryHistoryCount_ = 0;
  uint8_t batteryHistoryNext_ = 0;
  uint32_t lastBatteryHistoryMs_ = 0;
  bool climateReadPending_ = false;
  uint32_t climateReadReadyMs_ = 0;
};

class Buzzer {
 public:
  void begin();
  void configure(uint8_t volumePercent, uint8_t pitchPercent,
                 uint16_t highNoteBoostPercent = 200);
  void play(const String& toneName);
  void playConfigured(const String& toneName, const AppConfig& config);
  bool playRtttl(const String& rtttl);
  void news();
  void hourly();
  void notification();
  void confirm();
  void tick(uint32_t nowMs);

 private:
  struct ToneStep {
    uint16_t frequency;
    uint16_t durationMs;
    uint16_t gapMs;
  };

  void startMelody(const ToneStep* steps, uint8_t count);

  ToneStep melody_[96]{};
  uint8_t melodyCount_ = 0;
  uint8_t melodyIndex_ = 0;
  uint32_t nextToneMs_ = 0;
  uint32_t toneOffMs_ = 0;
  uint8_t volumePercent_ = 18;
  uint8_t pitchPercent_ = 100;
  uint16_t highNoteBoostPercent_ = 200;
  bool toneActive_ = false;
};

}  // namespace pixelos
