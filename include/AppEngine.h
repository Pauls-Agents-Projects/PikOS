#pragma once

#include <Arduino.h>

#include "Config.h"
#include "DataServices.h"
#include "I18n.h"
#include "LaMetricIcons.h"
#include "Peripherals.h"
#include "PixelDisplay.h"
#include "TextAnimator.h"

namespace pixelos {

enum class LocalApp : uint8_t {
  Time,
  Climate,
  Rss,
  Market,
  Energy,
  Battery,
  Calendar,
  Travel,
  Activity,
  Reddit,
  Counter,
  Sync
};

enum class TimedAlertKind : uint8_t { Alarm = 0, Timer };

struct TimedAlertEvent {
  TimedAlertKind kind = TimedAlertKind::Alarm;
  String text;
  String tone;
};

struct CounterStatus {
  bool countDown = true;
  bool running = false;
  bool finished = false;
  uint32_t durationSeconds = 0;
  uint32_t elapsedSeconds = 0;
  uint32_t remainingSeconds = 0;
};

class AppEngine {
 public:
  AppEngine(PixelDisplay& display, Peripherals& peripherals,
            DataServices& dataServices, ConfigStore& configStore,
            LaMetricIcons& iconStore);
  void begin();
  void tick(uint32_t nowMs, const AppConfig& config);
  // Alarm and timer completion remain active while the display task is
  // sleeping, showing a remote overlay, or waiting on another app.
  void tickBackground(uint32_t nowMs, const AppConfig& config);
  void next();
  void next(const AppConfig& config);
  void previous();
  void previous(const AppConfig& config);
  void resumeAutomaticAndAdvance();
  void resumeAutomaticAndAdvance(const AppConfig& config);
  // Enters the explicit Sync app immediately, preserving the current app and
  // automatic/manual mode for the return transition.
  void showRefreshAnimation();
  void showLatestNews(uint32_t headlineId);
  void showRainStarted(const NetatmoReading& reading);
  void showEnergyPrice();
  void showCalendarEvent(const CalendarEvent& event);
  bool toggleCounter(const AppConfig& config);
  bool resetCounter(const AppConfig& config);
  // Browser-only direct control. Unlike the physical helpers, this may open
  // and pin Counter before applying show/start/pause/toggle/reset.
  bool controlCounter(const String& action, const AppConfig& config);
  CounterStatus counterStatus() const;
  bool consumeTimedAlert(TimedAlertEvent& event);
  void showTimedAlert(const TimedAlertEvent& event);
  void notify(const String& text, uint32_t color, uint16_t durationMs,
              const String& animation,
              TinyIcon icon = TinyIcon::None, uint8_t iconLevel = 50,
              uint32_t customIconId = 0,
              bool scrollIconWithTicker = false);
  String currentAppName() const;
  bool paused() const { return paused_; }
  bool refreshAnimationActive() const;
  bool syncAppActive() const;
  void setSuspended(bool suspended, uint32_t nowMs);
  void renderStandbyClock(uint32_t nowMs, const AppConfig& config);
  bool browserTickerScene(uint32_t nowMs, BrowserTickerScene& scene) const;
  uint32_t copyDisplayFrame(CRGB* logicalFrame, uint8_t& brightness) const;
  uint32_t browserSceneKey() const;
  uint32_t appElapsedMs(uint32_t nowMs) const { return nowMs - appStartedMs_; }
  uint16_t renderFpsTenths() const { return renderFpsTenths_; }
  uint32_t renderedFrames() const { return renderedFrames_; }
  uint32_t missedFrameDeadlines() const { return missedFrameDeadlines_; }
  uint32_t maximumFrameGapUs() const { return maximumFrameGapUs_; }
  uint32_t estimatedLoopDurationMs(const AppConfig& config) const;
  uint32_t lastLoopDurationMs() const { return lastLoopDurationMs_; }
  uint8_t loopsUntilSync(const AppConfig& config) const;
  // Schedule with a little headroom so FastLED's 9 ms interrupt-disabled
  // WS2812 transfer still leaves a measured cadence of at least 60 FPS.
  static constexpr uint16_t targetFps() { return 64; }

 private:
  bool appEnabled(LocalApp app, const AppConfig& config) const;
  LocalApp adjacentEnabled(int direction, const AppConfig& config) const;
  LocalApp firstEnabled(const AppConfig& config) const;
  bool hasOnlineProviders(const AppConfig& config) const;
  bool wrapsForward(LocalApp from, LocalApp to) const;
  void beginSync(uint32_t nowMs, LocalApp returnApp, bool automatic);
  void finishSync(uint32_t nowMs, const AppConfig& config);
  void advanceAutomatic(uint32_t nowMs, const AppConfig& config);
  void startCurrent(uint32_t nowMs, const AppConfig& config,
                    bool continueApp = false);
  String buildMessage(LocalApp app, const AppConfig& config);
  String animationFor(LocalApp app, const AppConfig& config) const;
  uint32_t colorFor(LocalApp app) const;
  TinyIcon iconFor(LocalApp app) const;
  uint32_t customIconFor(LocalApp app, const AppConfig& config) const;
  uint8_t iconLevelFor(LocalApp app) const;
  void renderClock(uint32_t nowMs, const AppConfig& config);
  void renderRefreshAnimation(uint32_t nowMs);
  void renderModeIndicator(uint32_t nowMs, const AppConfig& config);
  void advanceClockResume(uint32_t nowMs, const AppConfig& config);
  void refreshAlarmSchedule(const AppConfig& config);
  void updateCounterState(uint32_t nowMs, const AppConfig& config);
  uint64_t counterElapsedAt(uint32_t nowMs) const;
  void renderCounter(uint32_t nowMs, const AppConfig& config);
  void queueTimedAlert(TimedAlertKind kind, const String& text,
                       const String& tone);

  PixelDisplay& display_;
  Peripherals& peripherals_;
  DataServices& dataServices_;
  ConfigStore& configStore_;
  LaMetricIcons& iconStore_;
  TextAnimator animator_;
  LocalApp current_ = LocalApp::Time;
  uint32_t appStartedMs_ = 0;
  uint32_t notificationUntilMs_ = 0;
  uint32_t refreshAnimationStartedMs_ = 0;
  uint32_t refreshAnimationUntilMs_ = 0;
  uint32_t lastRenderMs_ = 0;
  uint32_t nextFrameUs_ = 0;
  uint32_t previousFrameUs_ = 0;
  uint32_t frameWindowStartedUs_ = 0;
  uint32_t frameWindowFrames_ = 0;
  uint32_t renderedFrames_ = 0;
  uint32_t missedFrameDeadlines_ = 0;
  uint32_t maximumFrameGapUs_ = 0;
  uint16_t renderFpsTenths_ = 0;
  uint32_t syncQuietSinceMs_ = 0;
  uint32_t syncStartedAtMs_ = 0;
  uint32_t loopStartedMs_ = 0;
  uint32_t lastLoopDurationMs_ = 0;
  uint32_t estimatedLoopDurationMs_ = 0;
  uint32_t suspendedAtMs_ = 0;
  uint32_t lastStandbyMinute_ = UINT32_MAX;
  uint16_t rssIndex_ = 0;
  uint16_t marketIndex_ = 0;
  uint16_t clockTickerFaceIndex_ = 0;
  uint16_t calendarIndex_ = 0;
  uint16_t travelIndex_ = 0;
  uint8_t activityMetricCursor_ = 0;
  uint16_t redditIndex_ = 0;
  enum class ClimateMetric : uint8_t {
    Temperature = 0,
    Humidity,
    Wind,
    Rain
  };
  ClimateMetric currentClimateMetric_ = ClimateMetric::Temperature;
  uint8_t climateMetricCursor_ = 0;
  bool currentClimateNetatmo_ = false;
  String activeClockFaceKey_;
  String activeClockText_;
  uint32_t currentClockColor_ = 0xffffff;
  uint32_t currentClockIconId_ = 0;
  uint8_t currentClockHour_ = 0;
  uint8_t currentClockMinute_ = 0;
  float currentTemperatureC_ = NAN;
  float currentMarketChange_ = NAN;
  String currentMarketSymbol_;
  String currentRssSource_;
  String currentRssUrl_;
  uint32_t currentCalendarIconId_ = 0;
  uint32_t currentTravelIconId_ = 0;
  uint32_t currentTravelColor_ = 0x64d2ff;
  uint32_t currentActivityColor_ = 0x65ffb6;
  uint32_t currentRedditIconId_ = 0;
  float currentWeatherHumidity_ = NAN;
  float currentWeatherPressure_ = NAN;
  float currentWindSpeedKph_ = NAN;
  float currentWindGustKph_ = NAN;
  int16_t currentWindDirectionDegrees_ = -1;
  float currentRainRateMmHour_ = NAN;
  float currentRainLastHourMm_ = NAN;
  float currentRainLast24HoursMm_ = NAN;
  EnergyBand currentEnergyBand_ = EnergyBand::Average;
  float currentSparkline_[23]{};
  uint8_t currentSparklineCount_ = 0;
  int8_t currentSparklineMarker_ = -1;
  float currentEnergyLowThreshold_ = NAN;
  float currentEnergyHighThreshold_ = NAN;
  bool currentMarketOpen_ = false;
  static constexpr uint8_t kMaximumAlarms = 12;
  struct AlarmSlot {
    uint16_t minuteOfDay = 0;
    uint8_t weekdayMask = 0x7f;
    char label[49]{};
    char tone[16]{};
    uint32_t lastFiredDayKey = UINT32_MAX;
  };
  AlarmSlot alarms_[kMaximumAlarms]{};
  uint8_t alarmCount_ = 0;
  String cachedAlarmItems_;
  uint32_t lastBackgroundCounterTickMs_ = 0;
  uint32_t lastAlarmPollMs_ = 0;
  uint32_t lastAlarmEpochMinute_ = UINT32_MAX;
  // Enough for all twelve alarms plus a timer completing in the same minute.
  static constexpr uint8_t kTimedAlertQueueCapacity = kMaximumAlarms + 1;
  TimedAlertEvent timedAlertQueue_[kTimedAlertQueueCapacity];
  uint8_t timedAlertHead_ = 0;
  uint8_t timedAlertTail_ = 0;
  uint8_t timedAlertCount_ = 0;
  uint32_t timedAlertPresentationUntilMs_ = 0;
  bool counterConfigInitialized_ = false;
  bool counterCountDown_ = true;
  bool counterRunning_ = false;
  bool counterCompletionQueued_ = false;
  uint32_t counterConfiguredDurationSeconds_ = 300;
  uint64_t counterElapsedMs_ = 0;
  uint32_t counterLastUpdateMs_ = 0;
  bool paused_ = false;
  bool currentStarted_ = false;
  // Only a complete uninterrupted automatic rotation can advance the sync
  // cadence or teach the loop-duration estimator. Manual navigation and a
  // forced mid-loop Sync make the current rotation partial until its wrap.
  bool loopEligibleForCadence_ = true;
  bool firstSyncPending_ = true;
  bool syncAutomatic_ = false;
  bool syncReturnPaused_ = false;
  uint8_t loopsSinceSync_ = 0;
  LocalApp syncReturnApp_ = LocalApp::Time;
  enum class PendingNavigation : uint8_t { None, Previous, Next, Automatic };
  PendingNavigation pendingNavigation_ = PendingNavigation::None;
  mutable SemaphoreHandle_t mutex_ = nullptr;
};

bool isSleepTime(const AppConfig& config, time_t now);

}  // namespace pixelos
