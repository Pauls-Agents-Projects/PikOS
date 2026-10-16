#include <Arduino.h>
#include <atomic>
#include <cstring>
#include <esp_system.h>

#include "AppEngine.h"
#include "Config.h"
#include "CrashBreadcrumb.h"
#include "DataServices.h"
#include "LaMetricIcons.h"
#include "NetworkManager.h"
#include "Peripherals.h"
#include "PixelDisplay.h"
#include "PixelFont.h"
#include "RemoteDisplay.h"
#include "Version.h"
#include "WebPortal.h"

// The configuration portal parses a large JSON document and normalizes many
// String fields in one request. The ESP32 has ample RAM, while Arduino's
// default 8 KiB loop stack is too tight for that handler.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

namespace {

pixelos::ConfigStore configStore;
pixelos::PixelDisplay display;
pixelos::Peripherals peripherals;
pixelos::Buzzer buzzer;
pixelos::NetworkManager network;
pixelos::DataServices dataServices(configStore);
pixelos::LaMetricIcons iconStore;
pixelos::RemoteDisplay remoteDisplay;
pixelos::AppEngine appEngine(display, peripherals, dataServices, configStore,
                            iconStore);
pixelos::WebPortal webPortal(configStore, network, peripherals, dataServices,
                            remoteDisplay, appEngine, buzzer, iconStore,
                            display);

uint32_t lastRemoteFrame = 0;
std::atomic<uint32_t> wakeUntilMs{0};
std::atomic<uint32_t> lastInteractionMs{0};
std::atomic<bool> buttonTonePending{false};
uint32_t lastBrightnessMs = 0;
uint32_t lastRtcWriteMs = 0;
uint32_t lastCrashBreadcrumbMs = 0;
int32_t lastHourlyChime = -1;
bool displayBlanked = false;
pixelos::AppConfig runtimeConfig;
uint32_t runtimeConfigRevision = 0;
uint32_t lastLoopStartedMs = 0;
uint32_t maximumLoopGapMs = 0;
uint32_t maximumButtonLatencyMs = 0;
volatile bool displaySleeping = false;
bool localEngineSuspended = false;
pixelos::ButtonId wakeSuppressedLongButton = pixelos::ButtonId::Left;
uint32_t wakeSuppressedLongUntilMs = 0;
constexpr uint32_t kDisplayTaskStackBytes = 8192;
constexpr uint8_t kStandbyClockBrightness = 1;
StaticTask_t displayTaskBuffer;
StackType_t displayTaskStack[kDisplayTaskStackBytes / sizeof(StackType_t)];
TaskHandle_t displayTaskHandle = nullptr;

bool timeValid() {
  return time(nullptr) > 1672531200;
}

pixelos::OnlineProvider providerFromBreadcrumb(const char* name) {
  if (name == nullptr) return pixelos::OnlineProvider::None;
  for (uint8_t raw = static_cast<uint8_t>(pixelos::OnlineProvider::Rss);
       raw <= static_cast<uint8_t>(pixelos::OnlineProvider::Activity);
       ++raw) {
    const auto provider = static_cast<pixelos::OnlineProvider>(raw);
    if (strcmp(name, pixelos::crashProviderName(provider)) == 0) {
      return provider;
    }
  }
  return pixelos::OnlineProvider::None;
}

CRGB bootLogoColor(char cell, uint8_t frame) {
  switch (cell) {
    case 'W': return CRGB(245, 245, 247);
    case 'R': return CRGB(255, 59, 48);
    case 'G': return CRGB(52, 199, 89);
    case 'B': return CRGB(0, 122, 255);
    case 'C': {
      const uint8_t shimmer = static_cast<uint8_t>(210 + scale8(sin8(frame * 7), 45));
      return CRGB(shimmer, shimmer, 255);
    }
    default: return CRGB::Black;
  }
}

void showBootAnimation(const pixelos::AppConfig&) {
  buzzer.play("boot");
  // The same P-shaped clock/RGB mark is used by the browser and favicon. At
  // only eight pixels high, the white stem and bowl establish the letter,
  // while red/green/blue sub-pixel segments form its display-like outer arc.
  static constexpr char kLogo[8][9] = {
      ".WWWRR..", ".W...RR.", ".W...GG.", ".W...GG.",
      ".WWWBBB.", ".W...BB.", ".W......", ".W......",
  };
  constexpr int8_t kLogoX =
      (pixelos::hardware::kMatrixWidth - 8) / 2;
  for (uint8_t frame = 0; frame < 54; ++frame) {
    display.clear();
    for (uint8_t y = 0; y < 8; ++y) {
      for (uint8_t x = 0; x < 8; ++x) {
        const char cell = kLogo[y][x];
        if (cell == '.') continue;
        const uint8_t arrival = static_cast<uint8_t>(5 + y * 2 + (x & 3));
        const int16_t untilArrival = static_cast<int16_t>(arrival) - frame;
        int16_t drawX = kLogoX + x;
        uint8_t intensity = 255;
        if (untilArrival > 0) {
          // Before settling, coloured sub-pixels fly in from the right and
          // the white letter pixels from the left. This reads as assembly,
          // not as an unrelated loading bar.
          const int8_t direction = cell == 'W' ? -1 : 1;
          drawX += direction * min<int16_t>(11, untilArrival);
          if (drawX < 0 || drawX >= pixelos::hardware::kMatrixWidth) continue;
          intensity = static_cast<uint8_t>(
              constrain(255 - untilArrival * 18, 24, 255));
        } else if (frame > 28 && cell != 'W') {
          const uint8_t phase = cell == 'R' ? 0 : cell == 'G' ? 85 : 170;
          intensity = static_cast<uint8_t>(
              215 + scale8(sin8(frame * 4 + phase), 40));
        }
        CRGB color = bootLogoColor(cell, frame);
        color.nscale8_video(intensity);
        display.setPixel(drawX, y, color);
      }
    }

    // A tiny clock hand spins while the pixels assemble, then comes to rest
    // in the same pose as the scalable brand mark.
    if (frame >= 9) {
      static constexpr int8_t kHand[8][2] = {
          {3, 1}, {4, 1}, {4, 2}, {4, 3},
          {3, 3}, {2, 3}, {2, 2}, {2, 1},
      };
      const uint8_t hand = frame < 34 ? (frame / 3) & 7 : 3;
      display.setPixel(kLogoX + 3, 2, bootLogoColor('C', frame));
      display.setPixel(kLogoX + kHand[hand][0], kHand[hand][1],
                       bootLogoColor('C', frame));
    }
    display.show();
    buzzer.tick(millis());
    delay(24);
  }
  const uint32_t holdUntil = millis() + 360;
  while (static_cast<int32_t>(holdUntil - millis()) > 0) {
    buzzer.tick(millis());
    delay(4);
  }
}

String configuredButtonAction(const pixelos::AppConfig& config,
                              pixelos::ButtonId button,
                              pixelos::ButtonEventType type) {
  const bool longPress = type == pixelos::ButtonEventType::LongPressed;
  if (button == pixelos::ButtonId::Left) {
    return longPress ? config.buttonLongLeft : config.buttonShortLeft;
  }
  if (button == pixelos::ButtonId::Right) {
    return longPress ? config.buttonLongRight : config.buttonShortRight;
  }
  return longPress ? config.buttonLongMiddle : config.buttonShortMiddle;
}

bool performButtonAction(const String& action,
                         const pixelos::AppConfig& config,
                         const char* source) {
  if (action == "previous") {
    appEngine.previous(config);
  } else if (action == "next") {
    appEngine.next(config);
  } else if (action == "auto") {
    // Preserve Counter's natural centre-button ownership while it is open.
    if (!appEngine.toggleCounter(config)) {
      appEngine.resumeAutomaticAndAdvance(config);
    }
  } else if (action == "refresh") {
    Serial.printf("[input] %s refresh\n", source);
    appEngine.showRefreshAnimation();
  } else if (action == "device_info") {
    // A long centre press resets Counter when it is selected, matching the
    // pre-remapping behaviour. Everywhere else it shows the device address.
    if (!appEngine.resetCounter(config)) {
      const String address =
          network.connected()
              ? network.ipAddress()
              : network.accessPointName() + " PASS " +
                    network.accessPointPassword();
      appEngine.notify(address, 0x65ffb6, 9000, "ticker");
    }
  } else if (action == "counter_toggle") {
    return appEngine.controlCounter("toggle", config);
  } else if (action == "counter_reset") {
    return appEngine.controlCounter("reset", config);
  } else if (action == "none") {
    return false;
  } else {
    return false;
  }
  return true;
}

void handleButton(const pixelos::ButtonEvent& event, uint32_t nowMs,
                  const pixelos::AppConfig& config) {
  if (event.type == pixelos::ButtonEventType::LongPressed &&
      event.button == wakeSuppressedLongButton &&
      wakeSuppressedLongUntilMs != 0 &&
      wakeSuppressedLongUntilMs - nowMs <= 1500) {
    wakeSuppressedLongUntilMs = 0;
    return;
  }
  const bool wakingOnly = displaySleeping;
  lastInteractionMs.store(nowMs, std::memory_order_relaxed);
  wakeUntilMs.store(
      nowMs + static_cast<uint32_t>(config.buttonWakeMinutes) * 60000UL,
      std::memory_order_relaxed);

  pixelos::ButtonId button = event.button;
  if (config.orientation180) {
    if (button == pixelos::ButtonId::Left) {
      button = pixelos::ButtonId::Right;
    } else if (button == pixelos::ButtonId::Right) {
      button = pixelos::ButtonId::Left;
    }
  }

  // As on a desktop display, the first physical press only wakes the device.
  // It must not also skip an app before the user can see the screen.
  if (wakingOnly) {
    wakeSuppressedLongButton = event.button;
    wakeSuppressedLongUntilMs = nowMs + 1500;
    buttonTonePending.store(true, std::memory_order_release);
    return;
  }

  const String action = configuredButtonAction(config, button, event.type);
  if (performButtonAction(action, config, "physical")) {
    buttonTonePending.store(true, std::memory_order_release);
  }
}

void drainButtonEvents(const pixelos::AppConfig& config) {
  pixelos::ButtonEvent event;
  while (peripherals.popButtonEvent(event)) {
    const uint32_t handledAtMs = millis();
    // A long press intentionally spends 1.2 seconds waiting for the hold
    // threshold. Measure its dispatch from queue admission; short presses use
    // the raw electrical edge and therefore include debounce and sampling.
    const uint32_t latencyOrigin =
        event.type == pixelos::ButtonEventType::Pressed &&
                event.rawEdgeAtMs != 0
            ? event.rawEdgeAtMs
            : event.queuedAtMs;
    const uint32_t latency = handledAtMs - latencyOrigin;
    maximumButtonLatencyMs = max(maximumButtonLatencyMs, latency);
    if (latency >= 80) {
      Serial.printf("[perf] button latency=%lu ms, max loop gap=%lu ms\n",
                    static_cast<unsigned long>(latency),
                    static_cast<unsigned long>(maximumLoopGapMs));
    }
    handleButton(event, handledAtMs, config);
    peripherals.recordButtonApplied(event, millis());
  }
}

bool sleepRequested(const pixelos::AppConfig& config, uint32_t nowMs) {
  const bool schedule = pixelos::isSleepTime(config, time(nullptr));
  const uint32_t interactionMs =
      lastInteractionMs.load(std::memory_order_relaxed);
  const bool idle =
      config.sleepIdleEnabled && interactionMs != 0 &&
      nowMs - interactionMs >=
          static_cast<uint32_t>(config.sleepIdleMinutes) * 60000UL;
  return (schedule || idle) &&
         static_cast<int32_t>(
             wakeUntilMs.load(std::memory_order_relaxed) - nowMs) <= 0;
}

void serviceDisplay(uint32_t nowMs, const pixelos::AppConfig& config) {
  if (nowMs - lastBrightnessMs >= 100 && !displaySleeping) {
    lastBrightnessMs = nowMs;
    display.setBrightness(peripherals.desiredBrightness(config));
  }

  const bool requestedSleep = sleepRequested(config, nowMs);
  const bool remoteActive = remoteDisplay.active(nowMs, config);
  const bool shouldBlank =
      requestedSleep && !(remoteActive && config.remoteWakesDisplay);
  const bool suspendLocalEngine = shouldBlank || remoteActive;
  if (suspendLocalEngine != localEngineSuspended) {
    appEngine.setSuspended(suspendLocalEngine, nowMs);
    localEngineSuspended = suspendLocalEngine;
  }

  if (shouldBlank) {
    if (!displaySleeping) {
      displaySleeping = true;
      displayBlanked = false;
    }
    if (config.sleepDisplayMode == "clock") {
      // StandBy is intentionally the darkest usable physical setting. A
      // separate preference encouraged a night-time display bright enough to
      // disturb a dark room, so the value is now a product decision.
      display.setBrightness(kStandbyClockBrightness);
      appEngine.renderStandbyClock(nowMs, config);
      displayBlanked = false;
    } else if (!displayBlanked) {
      display.clear();
      display.show();
      displayBlanked = true;
    }
  } else {
    if (displaySleeping) {
      displaySleeping = false;
      display.setBrightness(peripherals.desiredBrightness(config));
    }
    if (remoteActive) {
      displayBlanked = false;
      if (lastRemoteFrame != remoteDisplay.frameSequence()) {
        lastRemoteFrame = remoteDisplay.frameSequence();
        display.showFrame(remoteDisplay.frame());
      }
    } else {
      displayBlanked = false;
      appEngine.tick(nowMs, config);
    }
  }
}

void displayTaskEntry(void*) {
  pixelos::AppConfig frameConfig;
  configStore.copyTo(frameConfig);
  uint32_t frameConfigRevision = configStore.revision();
  while (true) {
    const uint32_t nowMs = millis();
    const uint32_t latestRevision = configStore.revision();
    if (latestRevision != frameConfigRevision) {
      configStore.copyTo(frameConfig);
      frameConfigRevision = latestRevision;
    }
    peripherals.pollButtons(nowMs);
    drainButtonEvents(frameConfig);
    serviceDisplay(nowMs, frameConfig);
    vTaskDelay(1);
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(80);
  const esp_reset_reason_t resetReason = esp_reset_reason();
  pixelos::beginCrashBreadcrumb(resetReason);
  Serial.println();
  Serial.printf("Ulanzi PikOS %s\n", pixelos::kFirmwareVersion);
  Serial.printf("[boot] reset reason=%d\n",
                static_cast<int>(resetReason));
  const pixelos::CrashBreadcrumbSnapshot previous =
      pixelos::previousCrashBreadcrumb();
  if (previous.available) {
    Serial.printf(
        "[boot] previous context=%s app=%s provider=%s uptime=%lu ms%s\n",
        pixelos::crashContextName(previous.context), previous.app,
        previous.provider, static_cast<unsigned long>(previous.uptimeMs),
        previous.crashRelated ? " (crash-related reset)" : "");
  }

  if (!configStore.begin()) {
    Serial.println(F("[config] NVS initialization failed"));
  }
  configStore.copyTo(runtimeConfig);
  runtimeConfigRevision = configStore.revision();
  const pixelos::AppConfig& config = runtimeConfig;

  display.begin();
  display.setOrientation(config.orientation180);
  display.setBrightness(config.manualBrightness);
  buzzer.begin();
  buzzer.configure(config.buzzerVolume, config.buzzerPitchPercent,
                   config.buzzerHighNoteBoostPercent);
  showBootAnimation(config);

  peripherals.begin(config);
  if (peripherals.restoreClockFromRtc()) {
    Serial.println(F("[rtc] System clock restored from DS3231"));
  }
  network.begin(config);
  // Cache the settings document while the heap is least fragmented. The
  // listener is not serviced until loop(), so the remaining subsystems are
  // still fully initialised before any request handler can run.
  webPortal.begin();
  dataServices.begin();
  if (previous.crashRelated && previous.context == pixelos::CrashContext::Sync) {
    dataServices.quarantineProvider(
        providerFromBreadcrumb(previous.provider),
        "Paused after the previous sync panic");
  }
  iconStore.begin(config);
  remoteDisplay.begin(config);
  appEngine.begin();
  lastInteractionMs.store(millis(), std::memory_order_relaxed);
  // Rendering owns a higher-priority task on the Arduino core. It can preempt
  // synchronous HTTP parsing/writes at every 16.67 ms display deadline while
  // Wi-Fi and provider TLS continue on the other core.
  displayTaskHandle = xTaskCreateStaticPinnedToCore(
      displayTaskEntry, "pikos-display", kDisplayTaskStackBytes, nullptr, 2,
      displayTaskStack, &displayTaskBuffer, 1);
  if (displayTaskHandle == nullptr) {
    Serial.println(F("[perf] Could not start display task"));
  }
}

void loop() {
  uint32_t nowMs = millis();
  if (lastLoopStartedMs != 0) {
    maximumLoopGapMs = max(maximumLoopGapMs, nowMs - lastLoopStartedMs);
    webPortal.recordMainLoopGap(nowMs - lastLoopStartedMs);
  }
  lastLoopStartedMs = nowMs;
  network.tick(nowMs);
  if (network.consumeAccessPointAnnouncement()) {
    appEngine.notify(network.accessPointName() + " PASS " +
                         network.accessPointPassword(),
                     0x65ffb6, 18000, "ticker");
  }
  webPortal.tick(nowMs);
  // Web writes cooperatively render, but the remainder of this loop must use
  // current wall time rather than the timestamp from before a slow response.
  nowMs = millis();
  if (webPortal.consumeButtonWakeRequest()) {
    wakeUntilMs.store(
        nowMs + static_cast<uint32_t>(runtimeConfig.buttonWakeMinutes) *
                    60000UL,
        std::memory_order_relaxed);
    lastInteractionMs.store(nowMs, std::memory_order_relaxed);
  }
  const uint32_t latestRevision = configStore.revision();
  if (latestRevision != runtimeConfigRevision) {
    configStore.copyTo(runtimeConfig);
    runtimeConfigRevision = latestRevision;
  }
  const pixelos::AppConfig& config = runtimeConfig;
  if (lastCrashBreadcrumbMs == 0 ||
      nowMs - lastCrashBreadcrumbMs >= 1000UL) {
    lastCrashBreadcrumbMs = nowMs;
    const pixelos::SyncTelemetry sync = dataServices.syncTelemetry();
    const bool syncing = dataServices.refreshInProgress();
    const String app = appEngine.currentAppName();
    pixelos::recordCrashBreadcrumb(
        app.c_str(), sync.activeProvider,
        syncing ? pixelos::CrashContext::Sync
                : pixelos::CrashContext::Runtime,
        nowMs);
  }
  peripherals.tick(millis(), config);
  // All buzzer state and PWM writes remain on the main task. Physical button
  // rendering is still immediate, while a queued confirmation tone cannot
  // race news, calendar, hourly, or browser-triggered melodies.
  if (buttonTonePending.exchange(false, std::memory_order_acq_rel)) {
    buzzer.playConfigured(config.buttonTone, config);
  }
  buzzer.tick(nowMs);
  dataServices.tickLocal(nowMs);
  appEngine.tickBackground(nowMs, config);

  const bool syncAppActive = appEngine.syncAppActive();
  if (!iconStore.downloading() && !webPortal.clientActive()) {
    dataServices.tick(network.connected(), nowMs, syncAppActive);
  }
  if (syncAppActive && !dataServices.refreshInProgress()) {
    if (!webPortal.clientActive()) {
      iconStore.tick(network.connected(), config, nowMs);
    }
  }
  remoteDisplay.tick(config, nowMs);

  // Queue provider-triggered animations until the central cycle is fully
  // complete. Otherwise an RSS notification ticker can overlap the next TLS
  // provider even though the cycle itself began on the quiet battery screen.
  if (!dataServices.refreshInProgress() && !appEngine.syncAppActive()) {
    uint32_t newHeadlineId = 0;
    if (dataServices.consumeNewHeadlineEvent(newHeadlineId)) {
      if (config.rssBeepOnNew) {
        buzzer.playConfigured(config.newsTone, config);
      }
      appEngine.showLatestNews(newHeadlineId);
    }
    pixelos::NetatmoReading rainReading;
    if (dataServices.consumeRainStartEvent(rainReading)) {
      if (config.climateRainAlertEnabled) {
        buzzer.playConfigured(config.climateTone, config);
        appEngine.showRainStarted(rainReading);
      }
    }
    if (dataServices.consumeEnergyBandEvent()) {
      if (config.energyBeepOnChange) {
        buzzer.playConfigured(config.energyTone, config);
      }
      appEngine.showEnergyPrice();
    }
    pixelos::CalendarEvent calendarAlert;
    if (config.calendarAlertsEnabled &&
        dataServices.consumeCalendarAlertEvent(calendarAlert)) {
      buzzer.playConfigured(config.calendarTone, config);
      appEngine.showCalendarEvent(calendarAlert);
    }
  }

  if (!appEngine.syncAppActive()) {
    pixelos::TimedAlertEvent timedAlert;
    if (appEngine.consumeTimedAlert(timedAlert)) {
      buzzer.playConfigured(timedAlert.tone, config);
      wakeUntilMs.store(nowMs + 60000UL, std::memory_order_relaxed);
      appEngine.showTimedAlert(timedAlert);
    }
  }

  if (timeValid() &&
      (lastRtcWriteMs == 0 || nowMs - lastRtcWriteMs >= 21600000UL)) {
    lastRtcWriteMs = nowMs;
    peripherals.writeSystemClockToRtc();
  }

  if (timeValid() && config.hourlyChimeEnabled) {
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    const bool withinHours =
        config.hourlyChimeStart <= config.hourlyChimeEnd
            ? local.tm_hour >= config.hourlyChimeStart &&
                  local.tm_hour < config.hourlyChimeEnd
            : local.tm_hour >= config.hourlyChimeStart ||
                  local.tm_hour < config.hourlyChimeEnd;
    const int32_t chimeKey = local.tm_yday * 24 + local.tm_hour;
    if (withinHours && local.tm_min == 0 && chimeKey != lastHourlyChime) {
      buzzer.playConfigured(config.hourlyTone, config);
      lastHourlyChime = chimeKey;
    }
  }

  delay(1);
}
