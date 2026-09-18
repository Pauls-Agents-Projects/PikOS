#include "WebPortal.h"

#include <cerrno>
#include <Update.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_system.h>
#include <lwip/sockets.h>

#include "CrashBreadcrumb.h"
#include "TlsHeapGuard.h"
#include "WebUiGzip.h"
#include "PixelFont.h"
#include "Version.h"

namespace pixelos {

namespace {
constexpr const char* kBrowserConfigPath = "/browser-config.json";
uint32_t currentBootId = 0;

const char* providerName(OnlineProvider provider) {
  switch (provider) {
    case OnlineProvider::Rss: return "rss";
    case OnlineProvider::Netatmo: return "netatmo";
    case OnlineProvider::Market: return "market";
    case OnlineProvider::Energy: return "energy";
    case OnlineProvider::Calendar: return "calendar";
    case OnlineProvider::Travel: return "travel";
    case OnlineProvider::Reddit: return "reddit";
    case OnlineProvider::Activity: return "activity";
    case OnlineProvider::None:
    default: return "none";
  }
}
}

size_t ResponsiveWebServer::boundedWrite(const uint8_t* data, size_t length) {
  if (!_currentClient.connected() || _currentClient.fd() < 0) return 0;

  // WiFiClient::write retries EAGAIN internally in one-second steps for up to
  // roughly ten seconds. A delayed browser ACK therefore used to freeze the
  // display/buttons and could still return after only 512 bytes. Drive the
  // nonblocking socket directly: 512-byte pieces fit the TC001 send buffer,
  // ACKs are checked every millisecond, and the caller's total budget is a
  // real upper bound instead of merely being checked after a blocking call.
  constexpr size_t kChunkBytes = 512;
  const uint32_t startedAt = millis();
  size_t sent = 0;
  while (sent < length && millis() - startedAt < writeBudgetMs_) {
    const size_t chunk = min(kChunkBytes, length - sent);
    const ssize_t written =
        lwip_send(_currentClient.fd(), data + sent, chunk, MSG_DONTWAIT);
    if (written > 0) {
      sent += static_cast<size_t>(written);
      continue;
    }
    if (written < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS)) {
      delay(1);
      continue;
    }
    break;
  }
  if (sent != length) {
    Serial.printf("[web] Dropped slow client after %u/%u bytes\n",
                  static_cast<unsigned>(sent),
                  static_cast<unsigned>(length));
    _currentClient.stop();
  }
  return sent;
}

size_t ResponsiveWebServer::_currentClientWrite(const char* data,
                                                 size_t length) {
  return boundedWrite(reinterpret_cast<const uint8_t*>(data), length);
}

size_t ResponsiveWebServer::_currentClientWrite_P(PGM_P data,
                                                   size_t length) {
  return boundedWrite(reinterpret_cast<const uint8_t*>(data), length);
}

namespace {

const char* resetReasonText(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "SDIO";
    case ESP_RST_UNKNOWN:
    default:
      return "unknown";
  }
}

}  // namespace

WebPortal::WebPortal(ConfigStore& configStore, NetworkManager& network,
                     Peripherals& peripherals, DataServices& dataServices,
                     RemoteDisplay& remoteDisplay, AppEngine& appEngine,
                     Buzzer& buzzer, LaMetricIcons& iconStore,
                     PixelDisplay& display)
    : configStore_(configStore),
      network_(network),
      peripherals_(peripherals),
      dataServices_(dataServices),
      remoteDisplay_(remoteDisplay),
      appEngine_(appEngine),
      buzzer_(buzzer),
      iconStore_(iconStore),
      display_(display) {}

void WebPortal::begin() {
  currentBootId = esp_random();
  snprintf(adminSessionToken_, sizeof(adminSessionToken_), "%08lx%08lx%08lx%08lx",
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()),
           static_cast<unsigned long>(esp_random()));
  // Streaming ArduinoJson directly to WiFiClient performs many small writes
  // and can block the LED loop for seconds on a weak link. Keep one reusable
  // buffer so JSON is serialized in RAM and handed to the socket in one send.
  jsonResponseBuffer_.reserve(6144);
  // The browser configuration cache is required even when no LaMetric icons
  // are configured, so the portal owns its SPIFFS mount instead of relying on
  // another subsystem having mounted it first.
  SPIFFS.begin(true);
  const char* headers[] = {"X-Pixel-Key", "X-PikOS-Request", "Cookie"};
  server_.collectHeaders(headers, 3);

  server_.on("/", HTTP_GET, [this]() { sendRootPage(); });
  server_.on("/api/v1/status", HTTP_GET, [this]() { handleStatus(); });
  server_.on("/api/v1/performance", HTTP_GET,
             [this]() { handlePerformance(); });
  server_.on("/api/v1/resources", HTTP_GET,
             [this]() { handleResources(); });
  server_.on("/api/v1/battery-history", HTTP_GET,
             [this]() { handleBatteryHistory(); });
  server_.on("/api/v1/content", HTTP_GET,
             [this]() { handleContent(); });
  server_.on("/api/v1/performance/reset", HTTP_POST, [this]() {
    if (!checkAdminSession()) return;
    peripherals_.resetButtonTelemetry();
    maximumMainLoopGapMs_ = 0;
    server_.send(200, "application/json", "{\"ok\":true}");
  });
  server_.on("/api/v1/display", HTTP_GET, [this]() { handleDisplay(); });
  server_.on("/api/v1/scene", HTTP_GET, [this]() { handleScene(); });
  server_.on("/api/v1/icon-animation", HTTP_GET,
             [this]() { handleIconAnimation(); });
  server_.on("/api/v1/config", HTTP_GET, [this]() { handleGetConfig(); });
  server_.on("/api/v1/config", HTTP_POST, [this]() { handleSaveConfig(); });
  server_.on("/api/v1/brightness", HTTP_POST,
             [this]() { handleBrightnessPreview(); });
  server_.on("/api/v1/notify", HTTP_POST, [this]() { handleNotify(); });
  server_.on("/api/v1/frame", HTTP_POST, [this]() { handleFrame(); });
  server_.on("/api/v1/beep", HTTP_POST, [this]() {
    if (!checkAdminSession()) return;
    String tone = server_.arg("tone");
    if (tone.isEmpty()) {
      const String style = server_.arg("style");
      tone = style == "confirm" ? "click" : style;
    }
    AppConfig config = configStore_.snapshot();
    const int requestedVolume = server_.arg("volume").toInt();
    const int requestedPitch = server_.arg("pitch").toInt();
    const int requestedHighBoost = server_.arg("high_boost").toInt();
    if (server_.hasArg("volume")) {
      config.buzzerVolume =
          static_cast<uint8_t>(constrain(requestedVolume, 0, 100));
    }
    if (server_.hasArg("pitch")) {
      config.buzzerPitchPercent =
          static_cast<uint8_t>(constrain(requestedPitch, 80, 120));
    }
    if (server_.hasArg("high_boost")) {
      config.buzzerHighNoteBoostPercent =
          static_cast<uint16_t>(constrain(requestedHighBoost, 100, 300));
    }
    buzzer_.playConfigured(tone.isEmpty() ? "news" : tone, config);
    server_.send(200, "application/json", "{\"ok\":true}");
  });
  server_.on("/api/v1/rtttl", HTTP_POST, [this]() {
    if (!checkAdminSession()) return;
    DynamicJsonDocument document(2048);
    if (deserializeJson(document, server_.arg("plain"))) {
      server_.send(400, "application/json",
                   "{\"ok\":false,\"error\":\"invalid JSON\"}");
      return;
    }
    const String rtttl = document["rtttl"] | String("");
    const AppConfig config = configStore_.snapshot();
    buzzer_.configure(
        document["volume"] | config.buzzerVolume,
        document["pitch"] | config.buzzerPitchPercent,
        document["high_boost"] | config.buzzerHighNoteBoostPercent);
    if (!buzzer_.playRtttl(rtttl)) {
      server_.send(400, "application/json",
                   "{\"ok\":false,\"error\":\"invalid RTTTL\"}");
      return;
    }
    server_.send(200, "application/json", "{\"ok\":true}");
  });
  server_.on("/api/v1/refresh", HTTP_POST, [this]() {
    if (!checkAdminSession()) return;
    Serial.println(F("[input] browser refresh button"));
    appEngine_.showRefreshAnimation();
    buttonWakeRequested_ = true;
    server_.send(200, "application/json", "{\"ok\":true}");
  });
  server_.on("/api/v1/button", HTTP_POST,
             [this]() { handleButton(); });
  server_.on("/api/v1/counter", HTTP_POST,
             [this]() { handleCounter(); });
  server_.on("/api/v1/reboot", HTTP_POST, [this]() {
    if (!checkAdminSession()) return;
    recordCrashBreadcrumb(appEngine_.currentAppName().c_str(),
                          dataServices_.syncTelemetry().activeProvider,
                          CrashContext::Reboot, millis());
    server_.send(200, "application/json", "{\"ok\":true}");
    restartPending_ = true;
    restartAtMs_ = millis() + 600;
  });
  server_.on(
      "/api/v1/firmware", HTTP_POST,
      [this]() {
        const bool authorized = firmwareAuthorized_;
        firmwareAuthorized_ = false;
        if (!authorized) {
          server_.send(403, "application/json",
                       "{\"ok\":false,\"error\":\"browser session required\"}");
          return;
        }
        const bool success = !updateFailed_ && !Update.hasError();
        server_.send(success ? 200 : 500, "application/json",
                     success ? "{\"ok\":true}" :
                               "{\"ok\":false,\"error\":\"update failed\"}");
        if (success) {
          restartPending_ = true;
          restartAtMs_ = millis() + 1000;
        }
      },
      [this]() { handleFirmwareUpload(); });

  server_.onNotFound([this]() {
    if (network_.accessPointActive()) {
      sendRootPage();
    } else {
      server_.send(404, "application/json",
                   "{\"ok\":false,\"error\":\"not found\"}");
    }
  });
  if (!cacheBrowserConfig()) {
    Serial.println(F("[web] Could not cache browser config"));
  }
  server_.begin();
  listening_ = true;
  networkWasReady_ = network_.connected() || network_.accessPointActive();
}

void WebPortal::handlePerformance() {
  recordWebBreadcrumb();
  StaticJsonDocument<768> document;
  const ButtonTelemetry buttons = peripherals_.buttonTelemetry();
  document["target_fps"] = appEngine_.targetFps();
  document["render_fps"] = appEngine_.renderFpsTenths() / 10.0f;
  document["render_frames"] = appEngine_.renderedFrames();
  document["missed_deadlines"] = appEngine_.missedFrameDeadlines();
  document["max_frame_gap_us"] = appEngine_.maximumFrameGapUs();
  document["led_frames_sent"] = display_.transmittedFrames();
  document["led_frames_skipped"] = display_.skippedFrames();
  document["led_max_show_us"] = display_.maximumShowUs();
  document["data_refreshing"] = dataServices_.refreshInProgress();
  document["max_loop_gap_ms"] = maximumMainLoopGapMs_;
  document["button_event_count"] = buttons.appliedEvents;
  document["button_dropped_events"] = buttons.droppedEvents;
  document["button_last_latency_ms"] = buttons.lastLatencyMs;
  document["button_p95_latency_ms"] = buttons.p95LatencyMs;
  document["button_max_latency_ms"] = buttons.maximumLatencyMs;
  document["button_max_poll_gap_ms"] = buttons.maximumPollGapMs;
  sendJson(200, document);
}

void WebPortal::handleResources() {
  recordWebBreadcrumb();
  const AppConfig& config = configStore_.currentUnsafe();
  const ButtonTelemetry buttons = peripherals_.buttonTelemetry();
  const SyncTelemetry sync = dataServices_.syncTelemetry();
  const DataCacheTelemetry cache = dataServices_.cacheTelemetry();
  const SensorReadings sensors = peripherals_.readings();
  const size_t iconCacheBytes = iconStore_.cachedBytes();
  StaticJsonDocument<2304> document;
  document["ok"] = true;
  document["boot_id"] = currentBootId;
  document["uptime_seconds"] = millis() / 1000;
  document["free_heap_bytes"] = ESP.getFreeHeap();
  document["minimum_free_heap_bytes"] = ESP.getMinFreeHeap();
  document["largest_heap_block_bytes"] = ESP.getMaxAllocHeap();
  document["heap_total_bytes"] = ESP.getHeapSize();
  document["spiffs_used_bytes"] = SPIFFS.usedBytes();
  document["spiffs_total_bytes"] = SPIFFS.totalBytes();
  document["icon_cache_bytes"] = iconCacheBytes;
  document["icon_cache_count"] = iconStore_.cachedCount();
  document["online_store_bytes"] = cache.estimatedBytes;
  JsonObject storageByApp = document.createNestedObject("storage_by_app");
  storageByApp["icons"] = iconCacheBytes;
  storageByApp["rss"] = cache.rssBytes;
  storageByApp["climate"] = cache.climateBytes;
  storageByApp["market"] = cache.marketBytes;
  storageByApp["energy"] = cache.energyBytes;
  storageByApp["calendar"] = cache.calendarBytes;
  storageByApp["travel"] = cache.travelBytes;
  storageByApp["reddit"] = cache.redditBytes;
  storageByApp["activity"] = cache.activityBytes;
  document["storage_by_app_total_bytes"] =
      iconCacheBytes + cache.estimatedBytes;
  document["headline_count"] = cache.headlines;
  document["market_count"] = cache.marketQuotes;
  document["energy_point_count"] = cache.energyPoints;
  document["calendar_count"] = cache.calendarEvents;
  document["travel_count"] = cache.travelStops;
  document["reddit_count"] = cache.redditPosts;
  document["activity_count"] = cache.activityRecords;
  document["sync_active"] = dataServices_.refreshInProgress();
  document["sync_active_provider"] = providerName(sync.activeProvider);
  document["sync_cycles_started"] = sync.cyclesStarted;
  document["sync_cycles_completed"] = sync.cyclesCompleted;
  document["sync_cycles_aborted"] = sync.cyclesAborted;
  document["sync_failure_count"] = sync.providerFailures;
  document["sync_last_duration_ms"] = sync.lastSyncDurationMs;
  document["sync_average_duration_ms"] = sync.averageSyncDurationMs;
  document["sync_current_duration_ms"] = sync.currentSyncElapsedMs;
  document["tls_contention_count"] = tlsLeaseContentionCount();
  document["tls_maximum_wait_ms"] = tlsMaximumLeaseWaitMs();
  document["button_event_count"] = buttons.appliedEvents;
  document["button_dropped_events"] = buttons.droppedEvents;
  document["button_last_latency_ms"] = buttons.lastLatencyMs;
  document["button_p95_latency_ms"] = buttons.p95LatencyMs;
  document["button_max_latency_ms"] = buttons.maximumLatencyMs;
  document["button_max_poll_gap_ms"] = buttons.maximumPollGapMs;
  document["render_fps"] = appEngine_.renderFpsTenths() / 10.0f;
  document["render_frames"] = appEngine_.renderedFrames();
  document["missed_deadlines"] = appEngine_.missedFrameDeadlines();
  document["max_frame_gap_us"] = appEngine_.maximumFrameGapUs();
  document["max_loop_gap_ms"] = maximumMainLoopGapMs_;
  document["estimated_loop_duration_ms"] =
      appEngine_.estimatedLoopDurationMs(config);
  document["last_loop_duration_ms"] = appEngine_.lastLoopDurationMs();
  document["loops_until_sync"] = appEngine_.loopsUntilSync(config);
  document["battery_charging_likely"] = sensors.batteryChargingLikely;
  if (document.overflowed()) {
    document.clear();
    document["ok"] = false;
    document["error"] = "resource telemetry is too large";
    sendJson(507, document);
    return;
  }
  sendJson(200, document);
}

void WebPortal::handleBatteryHistory() {
  recordWebBreadcrumb();
  const BatteryHistorySnapshot history = peripherals_.batteryHistory();
  const SensorReadings sensors = peripherals_.readings();
  jsonDocument_.clear();
  JsonDocument& document = jsonDocument_;
  document["ok"] = true;
  document["health_available"] = false;
  document["health_reason"] =
      "voltage ADC only; no current, VBUS, charger status, or design capacity sensor";
  document["interval_seconds"] = history.intervalSeconds;
  document["maximum_samples"] = BatteryHistorySnapshot::kMaximumSamples;
  document["sample_count"] = history.sampleCount;
  document["newest_age_seconds"] = history.newestAgeSeconds;
  document["coverage_seconds"] =
      history.sampleCount > 1
          ? static_cast<uint32_t>(history.sampleCount - 1) *
                history.intervalSeconds
          : 0;
  document["charging_likely"] = sensors.batteryChargingLikely;
  document["current_percent"] = sensors.batteryPercent;
  document["current_raw"] = sensors.batteryRaw;
  document["observed_min_raw"] = sensors.batteryObservedMin;
  document["observed_max_raw"] = sensors.batteryObservedMax;
  document["profile_sample_count"] = sensors.batteryProfileSamples;

  JsonArray percent = document.createNestedArray("percent");
  JsonArray raw = document.createNestedArray("raw");
  JsonArray ageSeconds = document.createNestedArray("age_seconds");
  for (uint8_t index = 0; index < history.sampleCount; ++index) {
    percent.add(history.percent[index]);
    raw.add(history.raw[index]);
    ageSeconds.add(history.ageSeconds[index]);
  }

  const bool hasDrainEstimate = !isnan(history.drainPercentPerHour);
  if (hasDrainEstimate) {
    document["drain_percent_per_hour"] = history.drainPercentPerHour;
  } else {
    document["drain_percent_per_hour"] = nullptr;
  }
  if (!isnan(history.estimatedHoursRemaining)) {
    document["estimated_hours_remaining"] =
        history.estimatedHoursRemaining;
  } else {
    document["estimated_hours_remaining"] = nullptr;
  }
  document["trend"] =
      !hasDrainEstimate
          ? "insufficient-data"
          : history.drainPercentPerHour > 0.1f
                ? "discharging"
                : history.drainPercentPerHour < -0.1f ? "charging"
                                                       : "stable";
  if (document.overflowed()) {
    jsonDocument_.clear();
    document["ok"] = false;
    document["error"] = "battery history telemetry is too large";
    sendJson(507, document);
    return;
  }
  sendJson(200, document);
  jsonDocument_.clear();
}

void WebPortal::handleContent() {
  recordWebBreadcrumb();
  // Content is fetched only when a disclosure row is opened in the browser.
  // This keeps the normal status/resource polls compact and avoids copying
  // every String-backed provider cache while an animation is running.
  String app = server_.arg("app");
  app.toLowerCase();
  jsonDocument_.clear();
  JsonDocument& document = jsonDocument_;
  document["ok"] = true;
  document["app"] = app;
  JsonArray items = document.createNestedArray("items");
  String error;
  size_t count = 0;

  if (app == "rss" || app == "reddit") {
    Headline headline;
    const bool available = app == "rss"
                               ? dataServices_.headlineAt(0, headline, count,
                                                          error)
                               : dataServices_.redditAt(0, headline, count,
                                                        error);
    if (available) {
      const size_t limit = min<size_t>(count, 20);
      for (size_t index = 0; index < limit; ++index) {
        const bool found =
            app == "rss"
                ? dataServices_.headlineAt(index, headline, count, error)
                : dataServices_.redditAt(index, headline, count, error);
        if (!found) break;
        JsonObject item = items.createNestedObject();
        item["source"] = headline.source;
        item["title"] = headline.title;
        item["icon_id"] = headline.iconId;
        item["id"] = headline.id;
      }
    }
  } else if (app == "calendar") {
    CalendarEvent event;
    if (dataServices_.calendarAt(0, event, count, error)) {
      const size_t limit = min<size_t>(count, 20);
      for (size_t index = 0; index < limit; ++index) {
        if (!dataServices_.calendarAt(index, event, count, error)) break;
        JsonObject item = items.createNestedObject();
        item["source"] = event.source;
        item["title"] = event.title;
        item["starts_at"] = event.startsAt;
        item["ends_at"] = event.endsAt;
        item["all_day"] = event.allDay;
        item["icon_id"] = event.iconId;
      }
    }
  } else if (app == "travel") {
    TravelStopSnapshot stop;
    bool itemStale = true;
    if (dataServices_.travelAt(0, stop, count, error, itemStale)) {
      const size_t limit = min<size_t>(count, kMaximumTravelStopItems);
      const uint32_t now = static_cast<uint32_t>(time(nullptr));
      for (size_t index = 0; index < limit; ++index) {
        if (!dataServices_.travelAt(index, stop, count, error, itemStale)) {
          break;
        }
        JsonObject item = items.createNestedObject();
        item["label"] = stop.label;
        item["stop_id"] = stop.stopId;
        item["line"] = stop.line;
        item["icon_id"] = stop.iconId;
        item["fetched_at"] = stop.fetchedAt;
        item["stale"] = itemStale;
        item["error"] = stop.error;
        JsonArray departures = item.createNestedArray("departures");
        for (uint8_t departureIndex = 0;
             departureIndex < stop.departureCount &&
             departureIndex < kMaximumTravelDepartures;
             ++departureIndex) {
          const TravelDeparture& departure =
              stop.departures[departureIndex];
          JsonObject encoded = departures.createNestedObject();
          encoded["route"] = departure.route;
          encoded["destination"] = departure.destination;
          encoded["expected_at"] = departure.expectedAt;
          encoded["scheduled_at"] = departure.scheduledAt;
          encoded["realtime"] = departure.realtime;
          const uint32_t shownAt =
              itemStale && departure.scheduledAt != 0
                  ? departure.scheduledAt
                  : departure.expectedAt;
          encoded["minutes"] =
              shownAt <= now ? 0 : (shownAt - now + 59U) / 60U;
        }
      }
    }
    const OnlineStatus status = dataServices_.statusSnapshot();
    document["updated_at"] = status.travelUpdatedAt;
    document["stale"] = status.travelStale;
  } else if (app == "market") {
    MarketQuote quote;
    if (dataServices_.marketAt(0, quote, count, error)) {
      const size_t limit = min<size_t>(count, 20);
      for (size_t index = 0; index < limit; ++index) {
        if (!dataServices_.marketAt(index, quote, count, error)) break;
        JsonObject item = items.createNestedObject();
        item["symbol"] = quote.symbol;
        item["name"] = quote.name;
        item["currency"] = quote.currency;
        item["price"] = quote.price;
        item["change_percent"] = quote.percentChange;
        item["market_open"] = quote.marketOpen;
      }
    }
  } else if (app == "climate") {
    const AppConfig& config = configStore_.currentUnsafe();
    if (config.climateProvider == "local") {
      const SensorReadings local = peripherals_.readings();
      if (local.climateAvailable) {
        JsonObject item = items.createNestedObject();
        item["name"] = config.language == "et" ? "Seade" : "Device";
        item["provider"] = "local";
        if (isfinite(local.temperatureC))
          item["temperature_c"] = local.temperatureC;
        if (isfinite(local.humidityPercent))
          item["humidity_percent"] = local.humidityPercent;
      } else {
        error = config.language == "et" ? "Andmed puuduvad" : "No data";
      }
    } else {
      const NetatmoReading climate = dataServices_.netatmoSnapshot();
      if (climate.available || climate.rainAvailable ||
          climate.windAvailable) {
        JsonObject item = items.createNestedObject();
        item["name"] = climate.moduleName;
        item["provider"] = "netatmo";
        if (isfinite(climate.temperatureC))
          item["temperature_c"] = climate.temperatureC;
        if (isfinite(climate.humidityPercent))
          item["humidity_percent"] = climate.humidityPercent;
        if (isfinite(climate.pressureHpa))
          item["pressure_hpa"] = climate.pressureHpa;
        if (climate.rainAvailable) {
          item["rain_mm_h"] = climate.rainRateMmHour;
          item["rain_1h_mm"] = climate.rainLastHourMm;
          item["rain_24h_mm"] = climate.rainLast24HoursMm;
        }
        if (climate.windAvailable) {
          item["wind_kph"] = climate.windSpeedKph;
          item["gust_kph"] = climate.windGustKph;
          item["wind_degrees"] = climate.windDirectionDegrees;
        }
      } else {
        error = climate.error;
      }
    }
    count = items.size();
  } else if (app == "energy") {
    const EnergyPriceReading energy = dataServices_.energySnapshot();
    if (energy.available) {
      JsonObject item = items.createNestedObject();
      item["current_cents_kwh"] = energy.currentCentsKwh;
      item["average_cents_kwh"] = energy.averageCentsKwh;
      item["points"] = energy.pointsToday;
      item["next_change_at"] = energy.nextChangeAt;
      item["next_direction"] = energy.nextDirection;
    } else {
      error = energy.error;
    }
    count = items.size();
  } else if (app == "activity") {
    const ActivitySnapshot activity = dataServices_.activitySnapshot();
    if (activity.valid) {
      JsonObject item = items.createNestedObject();
      item["valid"] = true;
      item["stale"] = activity.stale;
      item["reconnect_required"] = activity.reconnectRequired;
      item["updated_at"] = activity.updatedAt;
      item["steps_today"] = activity.stepsToday;
      item["distance_today_m"] = activity.distanceTodayM;
      item["latest_started_at"] = activity.latestStartedAt;
      item["latest_distance_m"] = activity.latestDistanceM;
      item["latest_duration_sec"] = activity.latestDurationSec;
      item["latest_calories_kcal"] = activity.latestCaloriesKcal;
      item["latest_type"] = garminActivityTypeName(activity.latestType);
      item["latest_name"] = activity.latestName;
      if (!activity.error.isEmpty()) item["error"] = activity.error;
    } else {
      error = activity.error;
    }
    count = items.size();
  } else {
    jsonDocument_.clear();
    document["ok"] = false;
    document["error"] = "unknown app";
    sendJson(400, document);
    return;
  }

  document["count"] = count;
  if (!error.isEmpty()) document["error"] = error;
  if (document.overflowed()) {
    jsonDocument_.clear();
    document["ok"] = false;
    document["error"] = "stored content is too large";
    sendJson(507, document);
    return;
  }
  sendJson(200, document);
  networkHoldUntilMs_ = millis() + 1000;
}

void WebPortal::tick(uint32_t nowMs) {
  const bool networkReady =
      network_.connected() || network_.accessPointActive();
  const bool outboundTlsBusy =
      dataServices_.busy() || iconStore_.downloading();
  if (outboundTlsBusy) {
    // One ESP32 radio and a small lwIP socket pool serve both directions.
    // Accepting browser polls during an outbound handshake can accumulate
    // stale sockets; their 1.4 KiB receive buffers then fail exactly when TLS
    // needs its peak memory. Close the listener for the bounded provider/icon
    // operation and reopen it in the deliberate service gap. Display and
    // physical input remain fully live on their independent task.
    if (listening_) {
      server_.stop();
      listening_ = false;
    }
    networkWasReady_ = networkReady;
    return;
  }
  if (networkReady && (!networkWasReady_ || !listening_)) {
    // begin() can run a few milliseconds before the STA receives its address.
    // Rebind on each real network transition and after every bounded outbound
    // TLS job so the first listener cannot remain a silent pre-DHCP socket.
    server_.begin();
    listening_ = true;
  }
  networkWasReady_ = networkReady;
  if (!listening_) return;
  server_.handleClient();
  if (restartPending_ &&
      static_cast<int32_t>(nowMs - restartAtMs_) >= 0) {
    ESP.restart();
  }
}

bool WebPortal::consumeButtonWakeRequest() {
  const bool requested = buttonWakeRequested_;
  buttonWakeRequested_ = false;
  return requested;
}

void WebPortal::recordWebBreadcrumb() {
  const String app = appEngine_.currentAppName();
  recordCrashBreadcrumb(app.c_str(),
                        dataServices_.syncTelemetry().activeProvider,
                        CrashContext::Web, millis());
}

void WebPortal::sendRootPage() {
  String cookie = "pikos_session=";
  cookie += adminSessionToken_;
  cookie += "; Path=/; HttpOnly; SameSite=Strict";
  server_.sendHeader("Set-Cookie", cookie);
  server_.sendHeader("Content-Encoding", "gzip");
  server_.sendHeader("Cache-Control", "no-cache");
  server_.sendHeader("X-Content-Type-Options", "nosniff");
  server_.sendHeader("X-Frame-Options", "DENY");
  server_.sendHeader("Referrer-Policy", "no-referrer");
  server_.sendHeader("Permissions-Policy",
                     "camera=(), microphone=(), geolocation=()");
  server_.sendHeader(
      "Content-Security-Policy",
      "default-src 'self'; base-uri 'none'; object-src 'none'; "
      "frame-ancestors 'none'; form-action 'self'; "
      "script-src 'self' 'unsafe-inline' https://transport.tallinn.ee; "
      "style-src 'self' 'unsafe-inline'; "
      "img-src 'self' data: https://developer.lametric.com "
      "https://www.google.com; "
      "connect-src 'self' https://developer.lametric.com");
  // The settings bundle is large enough that a single retransmission on a
  // weak 2.4 GHz link is normal. Display and physical input run separately,
  // so this bounded reliability window cannot stall animation or buttons.
  server_.setWriteBudgetMs(1500);
  server_.send_P(200, "text/html; charset=utf-8",
                 reinterpret_cast<PGM_P>(kWebUiGzip), kWebUiGzipSize);
  server_.setWriteBudgetMs(200);
  networkHoldUntilMs_ = millis() + 10000;
}

bool WebPortal::validRequestHost() {
  String host = server_.hostHeader();
  host.trim();
  host.toLowerCase();
  const int port = host.indexOf(':');
  if (port >= 0) host.remove(port);
  if (host.isEmpty()) return false;

  String configured = network_.hostName();
  configured.toLowerCase();
  String localAddress = network_.ipAddress();
  localAddress.toLowerCase();
  return host == localAddress || host == configured ||
         host == configured + ".local" ||
         (network_.accessPointActive() && host == "192.168.4.1");
}

bool WebPortal::hasAdminSession() {
  // Reject DNS-rebinding origins before checking the cookie. A remote web
  // page can cause a browser to resolve its own hostname to a private IP; it
  // must not be able to reuse PikOS's deliberately login-free LAN session.
  if (!validRequestHost()) return false;
  if (server_.header("X-PikOS-Request") != "1") return false;
  const String expected = String("pikos_session=") + adminSessionToken_;
  const String cookies = server_.header("Cookie");
  int start = 0;
  while (start < static_cast<int>(cookies.length())) {
    int end = cookies.indexOf(';', start);
    if (end < 0) end = cookies.length();
    String item = cookies.substring(start, end);
    item.trim();
    if (item.length() == expected.length()) {
      uint8_t difference = 0;
      for (size_t index = 0; index < item.length(); ++index) {
        difference |= static_cast<uint8_t>(item[index] ^ expected[index]);
      }
      if (difference == 0) return true;
    }
    start = end + 1;
  }
  return false;
}

bool WebPortal::checkAdminSession() {
  if (hasAdminSession()) return true;
  server_.send(403, "application/json",
               "{\"ok\":false,\"error\":\"browser session required\"}");
  return false;
}

bool WebPortal::checkApiKey() {
  const AppConfig config = configStore_.snapshot();
  String supplied = server_.header("X-Pixel-Key");
  if (supplied.isEmpty()) {
    supplied = server_.arg("key");
  }
  if (supplied.length() == config.apiKey.length() && !supplied.isEmpty()) {
    uint8_t difference = 0;
    for (size_t index = 0; index < supplied.length(); ++index) {
      difference |=
          static_cast<uint8_t>(supplied[index] ^ config.apiKey[index]);
    }
    if (difference == 0) return true;
  }
  server_.send(401, "application/json",
               "{\"ok\":false,\"error\":\"invalid API key\"}");
  return false;
}

bool WebPortal::checkApiKeyOrAdminSession() {
  return hasAdminSession() || checkApiKey();
}

bool WebPortal::cacheBrowserConfig() {
  const String output = configStore_.browserJson();
  if (output.isEmpty()) {
    Serial.println(F("[web] Browser config serialization returned empty"));
    return false;
  }
  // This is a regenerable derivative of the NVS configuration. Writing the
  // final cache directly is both simpler and more reliable on SPIFFS than a
  // remove/rename pair; a short/failed write is detected below and retried on
  // the first GET without risking the authoritative settings.
  File file = SPIFFS.open(kBrowserConfigPath, FILE_WRITE);
  if (!file) return false;
  const size_t written = file.write(
      reinterpret_cast<const uint8_t*>(output.c_str()), output.length());
  file.flush();
  file.close();
  if (written != output.length()) {
    SPIFFS.remove(kBrowserConfigPath);
    return false;
  }
  Serial.printf("[web] Browser config cached: %u bytes\n",
                static_cast<unsigned>(written));
  return true;
}

bool WebPortal::sendCachedBrowserConfig() {
  File file = SPIFFS.open(kBrowserConfigPath, FILE_READ);
  if (!file) return false;
  size_t length = file.size();
  if (length == 0) {
    file.close();
    if (!cacheBrowserConfig()) return false;
    file = SPIFFS.open(kBrowserConfigPath, FILE_READ);
    if (!file) return false;
    length = file.size();
    if (length == 0) {
      file.close();
      return false;
    }
  }
  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("Connection", "close");
  server_.setWriteBudgetMs(500);
  server_.setContentLength(length);
  server_.send(200, "application/json", "");
  uint8_t chunk[512];
  size_t sent = 0;
  while (sent < length) {
    const size_t count = file.read(chunk, min(sizeof(chunk), length - sent));
    if (count == 0) break;
    server_.sendContent(reinterpret_cast<const char*>(chunk), count);
    WiFiClient client = server_.client();
    if (!client || !client.connected()) break;
    sent += count;
  }
  file.close();
  server_.setWriteBudgetMs(200);
  return sent == length;
}

void WebPortal::sendJson(int status, JsonDocument& document) {
  jsonResponseBuffer_ = "";
  serializeJson(document, jsonResponseBuffer_);
  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("Connection", "close");
  // A weak-link ACK can easily arrive later than 75 ms. Keep tiny control
  // responses bounded, but give status/content payloads enough time to finish
  // instead of returning syntactically truncated JSON. Rendering and physical
  // input continue independently on the high-priority display task.
  const size_t bytes = jsonResponseBuffer_.length();
  server_.setWriteBudgetMs(bytes > 8192 ? 1200 : bytes > 2048 ? 500 : 200);
  server_.send(status, "application/json", jsonResponseBuffer_);
  server_.setWriteBudgetMs(200);
}

void WebPortal::handleStatus() {
  recordWebBreadcrumb();
  server_.sendHeader("Connection", "close");
  if (dataServices_.refreshInProgress()) {
    String compact =
        "{\"ok\":true,\"data_refreshing\":true,\"sync_visual_active\":";
    compact += appEngine_.refreshAnimationActive() ? "true" : "false";
    compact += ",\"sync_completed\":" +
               String(dataServices_.syncCompleted());
    compact += ",\"sync_total\":" + String(dataServices_.syncTotal());
    compact += ",\"sync_progress\":" +
               String(dataServices_.syncProgressPercent()) + "}";
    server_.sendHeader("Connection", "close");
    server_.send(200, "application/json", compact);
    return;
  }
  // Status and settings publication both run on loopTask. Reading the
  // published value in place avoids reallocating every AppConfig String on
  // each browser poll; the display task still receives revisioned copies.
  const AppConfig& config = configStore_.currentUnsafe();
  const SensorReadings sensors = peripherals_.readings();
  const OnlineStatus online = dataServices_.statusSnapshot();
  jsonDocument_.clear();
  JsonDocument& document = jsonDocument_;
  const time_t now = time(nullptr);
  tm local{};
  localtime_r(&now, &local);
  char localTime[40];
  strftime(localTime, sizeof(localTime), "%Y-%m-%d %H:%M:%S %Z", &local);
  document["ok"] = true;
  document["data_refreshing"] = dataServices_.refreshInProgress();
  document["sync_visual_active"] = appEngine_.refreshAnimationActive();
  document["sync_completed"] = dataServices_.syncCompleted();
  document["sync_total"] = dataServices_.syncTotal();
  document["sync_progress"] = dataServices_.syncProgressPercent();
  document["firmware_version"] = kFirmwareVersion;
  document["reset_reason"] = resetReasonText(esp_reset_reason());
  const CrashBreadcrumbSnapshot previous = previousCrashBreadcrumb();
  JsonObject previousBoot = document.createNestedObject("previous_boot");
  previousBoot["available"] = previous.available;
  previousBoot["crash_related"] = previous.crashRelated;
  previousBoot["reset_reason"] = resetReasonText(previous.resetReason);
  if (previous.available) {
    previousBoot["context"] = crashContextName(previous.context);
    previousBoot["app"] = previous.app;
    previousBoot["sync_provider"] = previous.provider;
    previousBoot["uptime_ms"] = previous.uptimeMs;
    previousBoot["sequence"] = previous.sequence;
  }
  CrashBreadcrumbSnapshot recentCrashes[4];
  const size_t recentCrashCount =
      recentCrashBreadcrumbs(recentCrashes, 4);
  JsonArray crashHistory = document.createNestedArray("recent_crashes");
  for (size_t index = 0; index < recentCrashCount; ++index) {
    const CrashBreadcrumbSnapshot& crash = recentCrashes[index];
    JsonObject item = crashHistory.createNestedObject();
    item["reset_reason"] = resetReasonText(crash.resetReason);
    item["context"] = crashContextName(crash.context);
    item["app"] = crash.app;
    item["sync_provider"] = crash.provider;
    item["uptime_ms"] = crash.uptimeMs;
    item["sequence"] = crash.sequence;
  }
  document["name"] = config.deviceName;
  document["ip"] = network_.ipAddress();
  document["wifi_connected"] = network_.connected();
  document["ap_active"] = network_.accessPointActive();
  document["ap_name"] = network_.accessPointName();
  document["rssi"] = network_.rssi();
  document["wifi_disconnect_reason"] = network_.lastDisconnectReason();
  document["wifi_disconnect_message"] = network_.lastDisconnectMessage();
  document["current_app"] = appEngine_.currentAppName();
  document["app_paused"] = appEngine_.paused();
  const CounterStatus counter = appEngine_.counterStatus();
  document["counter_count_down"] = counter.countDown;
  document["counter_running"] = counter.running;
  document["counter_finished"] = counter.finished;
  document["counter_duration_seconds"] = counter.durationSeconds;
  document["counter_elapsed_seconds"] = counter.elapsedSeconds;
  document["counter_remaining_seconds"] = counter.remainingSeconds;
  document["light_raw"] = sensors.lightRaw;
  document["brightness"] = peripherals_.desiredBrightness(config);
  document["battery_raw"] = sensors.batteryRaw;
  document["battery_percent"] = sensors.batteryPercent;
  document["battery_observed_min"] = sensors.batteryObservedMin;
  document["battery_observed_max"] = sensors.batteryObservedMax;
  document["battery_profile_samples"] = sensors.batteryProfileSamples;
  document["battery_trend"] =
      sensors.batteryTrend > 0
          ? "rising"
          : (sensors.batteryTrend < 0 ? "falling" : "stable");
  document["battery_charging_likely"] = sensors.batteryChargingLikely;
  document["battery_saver_active"] =
      peripherals_.batterySaverActive(config);
  document["climate_available"] = sensors.climateAvailable;
  document["climate_provider"] = config.climateProvider;
  document["temperature_c"] =
      sensors.climateAvailable ? sensors.temperatureC : 0.0f;
  document["humidity_percent"] =
      sensors.climateAvailable ? sensors.humidityPercent : 0.0f;
  document["headline_count"] = online.headlineCount;
  document["rss_error"] = online.rssError;
  document["rss_updated_at"] = online.rssUpdatedAt;
  document["netatmo_available"] = online.netatmo.available;
  document["netatmo_temperature_c"] =
      online.netatmo.available ? online.netatmo.temperatureC : 0.0f;
  document["netatmo_humidity_percent"] =
      online.netatmo.available ? online.netatmo.humidityPercent : 0.0f;
  document["netatmo_pressure_hpa"] =
      online.netatmo.available ? online.netatmo.pressureHpa : 0.0f;
  document["netatmo_rain_available"] = online.netatmo.rainAvailable;
  document["netatmo_rain_mm_h"] =
      online.netatmo.rainAvailable ? online.netatmo.rainRateMmHour : 0.0f;
  document["netatmo_rain_1h_mm"] =
      online.netatmo.rainAvailable ? online.netatmo.rainLastHourMm : 0.0f;
  document["netatmo_rain_24h_mm"] =
      online.netatmo.rainAvailable ? online.netatmo.rainLast24HoursMm : 0.0f;
  document["netatmo_wind_available"] = online.netatmo.windAvailable;
  document["netatmo_wind_kph"] =
      online.netatmo.windAvailable ? online.netatmo.windSpeedKph : 0.0f;
  document["netatmo_gust_kph"] =
      online.netatmo.windAvailable ? online.netatmo.windGustKph : 0.0f;
  document["netatmo_wind_degrees"] = online.netatmo.windDirectionDegrees;
  document["netatmo_module_name"] = online.netatmo.moduleName;
  document["netatmo_error"] = online.netatmo.error;
  document["netatmo_updated_at"] = online.netatmoUpdatedAt;
  document["market_count"] = online.marketCount;
  document["market_chart_points"] = online.marketChartPoints;
  document["market_error"] = online.marketError;
  document["market_updated_at"] = online.marketUpdatedAt;
  document["energy_available"] = online.energy.available;
  document["energy_current_cents_kwh"] =
      online.energy.available ? online.energy.currentCentsKwh : 0.0f;
  document["energy_average_cents_kwh"] =
      online.energy.available ? online.energy.averageCentsKwh : 0.0f;
  document["energy_band"] =
      online.energy.band == EnergyBand::Low
          ? "low"
          : (online.energy.band == EnergyBand::High ? "high" : "average");
  document["energy_next_direction"] = online.energy.nextDirection;
  document["energy_next_change_at"] = online.energy.nextChangeAt;
  document["energy_points_today"] = online.energy.pointsToday;
  document["energy_chart_points"] = online.energy.sparklineCount;
  document["energy_error"] = online.energy.error;
  document["energy_updated_at"] = online.energyUpdatedAt;
  document["calendar_count"] = online.calendarCount;
  document["calendar_error"] = online.calendarError;
  document["calendar_updated_at"] = online.calendarUpdatedAt;
  document["travel_count"] = online.travelCount;
  document["travel_error"] = online.travelError;
  document["travel_stale"] = online.travelStale;
  document["reddit_count"] = online.redditCount;
  document["reddit_error"] = online.redditError;
  document["reddit_updated_at"] = online.redditUpdatedAt;
  document["activity_connected"] = online.activityConnected;
  document["activity_records"] = online.activity.valid ? 1 : 0;
  document["activity_valid"] = online.activity.valid;
  document["activity_stale"] = online.activity.stale;
  document["activity_reconnect_required"] =
      online.activity.reconnectRequired;
  document["activity_updated_at"] = online.activityUpdatedAt;
  document["activity_steps_today"] = online.activity.stepsToday;
  document["activity_distance_today_m"] =
      online.activity.distanceTodayM;
  document["activity_latest_started_at"] =
      online.activity.latestStartedAt;
  document["activity_latest_distance_m"] =
      online.activity.latestDistanceM;
  document["activity_latest_duration_sec"] =
      online.activity.latestDurationSec;
  document["activity_latest_calories_kcal"] =
      online.activity.latestCaloriesKcal;
  document["activity_latest_type"] =
      garminActivityTypeName(online.activity.latestType);
  document["activity_latest_name"] = online.activity.latestName;
  document["activity_error"] = online.activityError;
  document["lametric_icons_cached"] = iconStore_.cachedCount();
  document["lametric_icon_error"] = iconStore_.lastError();
  document["remote_active"] = remoteDisplay_.active(millis(), config);
  document["remote_protocol"] = remoteDisplay_.lastProtocol();
  document["remote_frame_sequence"] = remoteDisplay_.frameSequence();
  document["led_frames_sent"] = display_.transmittedFrames();
  document["led_frames_skipped"] = display_.skippedFrames();
  document["led_max_show_us"] = display_.maximumShowUs();
  document["render_target_fps"] = appEngine_.targetFps();
  document["render_fps"] = appEngine_.renderFpsTenths() / 10.0f;
  document["render_frames"] = appEngine_.renderedFrames();
  document["render_missed_deadlines"] =
      appEngine_.missedFrameDeadlines();
  document["render_max_frame_gap_us"] = appEngine_.maximumFrameGapUs();
  document["estimated_loop_duration_ms"] =
      appEngine_.estimatedLoopDurationMs(config);
  document["last_loop_duration_ms"] = appEngine_.lastLoopDurationMs();
  document["loops_until_sync"] = appEngine_.loopsUntilSync(config);
  document["uptime_seconds"] = millis() / 1000;
  document["free_heap_bytes"] = ESP.getFreeHeap();
  document["largest_heap_block_bytes"] = ESP.getMaxAllocHeap();
  document["epoch"] = static_cast<uint32_t>(now);
  document["local_time"] = localTime;
  sendJson(200, document);
  jsonDocument_.clear();
}

void WebPortal::handleDisplay() {
  recordWebBreadcrumb();
  server_.sendHeader("Connection", "close");
  if (dataServices_.busy()) {
    // The ESP32 can keep the local display and buttons moving during HTTPS,
    // but pushing an inbound preview frame while the same radio is receiving
    // a provider response can exhaust lwIP's small socket-buffer pool. The
    // browser keeps the previous keyframe and resumes after the job window.
    server_.send(204, "text/plain", "");
    return;
  }
  constexpr size_t kPixelCount =
      hardware::kMatrixWidth * hardware::kMatrixHeight;
  CRGB logical[kPixelCount];
  uint8_t frameBrightness = 0;
  const uint32_t frameSequence =
      appEngine_.copyDisplayFrame(logical, frameBrightness);
  if (server_.arg("format") == "delta565") {
    // The browser keeps a logical frame and animates between sparse keyframes
    // with requestAnimationFrame. Static pixels no longer cross Wi-Fi on
    // every poll, while moving text gets more frequent, smoother targets.
    const uint32_t sequence = frameSequence;

    const uint32_t since = strtoul(server_.arg("since").c_str(), nullptr, 10);
    const bool canDelta = twinBaselineValid_ &&
                          since == twinBaselineSequence_;
    uint16_t changed = 0;
    if (canDelta) {
      for (size_t index = 0; index < kPixelCount; ++index) {
        if (logical[index] != twinBaseline_[index]) ++changed;
      }
    }

    // Header: type, brightness, big-endian frame sequence. A full frame then
    // has 256 RGB565 values. A delta adds a big-endian count followed by
    // index/RGB565 triples. Zero-change frames need only the six-byte header.
    uint8_t response[8 + kPixelCount * 3];
    response[1] = frameBrightness;
    response[2] = static_cast<uint8_t>(sequence >> 24);
    response[3] = static_cast<uint8_t>(sequence >> 16);
    response[4] = static_cast<uint8_t>(sequence >> 8);
    response[5] = static_cast<uint8_t>(sequence);
    size_t length = 6;
    if (!canDelta || changed >= 170) {
      response[0] = 0;
      size_t offset = 6;
      for (const CRGB& color : logical) {
        const uint16_t packed =
            (static_cast<uint16_t>(color.r & 0xf8) << 8) |
            (static_cast<uint16_t>(color.g & 0xfc) << 3) |
            (static_cast<uint16_t>(color.b) >> 3);
        response[offset++] = static_cast<uint8_t>(packed >> 8);
        response[offset++] = static_cast<uint8_t>(packed);
      }
      length = offset;
    } else if (changed == 0) {
      response[0] = 2;
    } else {
      response[0] = 1;
      response[6] = static_cast<uint8_t>(changed >> 8);
      response[7] = static_cast<uint8_t>(changed);
      size_t offset = 8;
      for (size_t index = 0; index < kPixelCount; ++index) {
        const CRGB color = logical[index];
        if (color == twinBaseline_[index]) continue;
        const uint16_t packed =
            (static_cast<uint16_t>(color.r & 0xf8) << 8) |
            (static_cast<uint16_t>(color.g & 0xfc) << 3) |
            (static_cast<uint16_t>(color.b) >> 3);
        response[offset++] = static_cast<uint8_t>(index);
        response[offset++] = static_cast<uint8_t>(packed >> 8);
        response[offset++] = static_cast<uint8_t>(packed);
      }
      length = offset;
    }

    memcpy(twinBaseline_, logical, sizeof(logical));
    twinBaselineSequence_ = sequence;
    twinBaselineValid_ = true;
    server_.sendHeader("Cache-Control", "no-store");
    server_.sendHeader("Connection", "close");
    server_.setContentLength(length);
    server_.send(200, "application/octet-stream", "");
    server_.sendContent(reinterpret_cast<const char*>(response), length);
    return;
  }

  if (server_.arg("format") == "rgb565") {
    // The browser preview only needs five or six bits per channel.  A binary
    // frame is 513 bytes instead of roughly 2.3 KiB of JSON, leaving far more
    // airtime for settings and online-data requests.
    uint8_t frame[1 + hardware::kMatrixWidth * hardware::kMatrixHeight * 2];
    frame[0] = frameBrightness;
    size_t offset = 1;
    for (uint8_t y = 0; y < hardware::kMatrixHeight; ++y) {
      for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
        const CRGB color = logical[y * hardware::kMatrixWidth + x];
        const uint16_t packed =
            (static_cast<uint16_t>(color.r & 0xf8) << 8) |
            (static_cast<uint16_t>(color.g & 0xfc) << 3) |
            (static_cast<uint16_t>(color.b) >> 3);
        frame[offset++] = static_cast<uint8_t>(packed >> 8);
        frame[offset++] = static_cast<uint8_t>(packed & 0xff);
      }
    }
    server_.sendHeader("Cache-Control", "no-store");
    server_.sendHeader("Connection", "close");
    server_.setContentLength(sizeof(frame));
    server_.send(200, "application/octet-stream", "");
    server_.sendContent(reinterpret_cast<const char*>(frame), sizeof(frame));
    return;
  }

  // A compact numeric array is cheaper than 256 JSON strings and can be
  // painted directly into the browser's 32x8 ImageData buffer.
  String output;
  output.reserve(2300);
  output = "{\"ok\":true,\"brightness\":";
  output += String(frameBrightness);
  output += ",\"pixels\":[";
  for (uint8_t y = 0; y < hardware::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
      if (x != 0 || y != 0) {
        output += ',';
      }
      const CRGB color = logical[y * hardware::kMatrixWidth + x];
      const uint32_t packed =
          (static_cast<uint32_t>(color.r) << 16) |
          (static_cast<uint32_t>(color.g) << 8) | color.b;
      output += String(packed);
    }
  }
  output += "]}";
  server_.sendHeader("Connection", "close");
  server_.send(200, "application/json", output);
}

void WebPortal::handleScene() {
  recordWebBreadcrumb();
  server_.sendHeader("Connection", "close");
  if (dataServices_.busy()) {
    server_.send(503, "application/json",
                 "{\"ok\":false,\"error\":\"radio busy\"}");
    return;
  }
  const uint32_t since =
      strtoul(server_.arg("since").c_str(), nullptr, 10);
  const uint32_t currentKey = appEngine_.browserSceneKey();
  if (currentKey != 0 && since == currentKey) {
    server_.send(204, "text/plain", "");
    return;
  }
  BrowserTickerScene scene;
  const uint32_t nowMs = millis();
  if (!appEngine_.browserTickerScene(nowMs, scene)) {
    server_.send(409, "application/json",
                 "{\"ok\":false,\"client_render\":false}");
    return;
  }
  CRGB iconPixels[64]{};
  bool hasIconPixels = false;
  if (scene.customIconId != 0) {
    hasIconPixels =
        iconStore_.copyFrame(scene.customIconId, iconPixels, nowMs);
  }
  if (!hasIconPixels && scene.icon != TinyIcon::None &&
      scene.iconLabel.isEmpty()) {
    // Render a procedural icon into a scratch matrix. This never calls
    // FastLED.show(), so producing a browser scene cannot touch the physical
    // display or stall its refresh schedule.
    PixelDisplay scratch;
    scratch.clear();
    if (scene.icon == TinyIcon::Clock && scene.analogHour < 24U &&
        scene.analogMinute < 60U) {
      scratch.drawAnalogClock(0, 0, scene.color, scene.analogHour,
                              scene.analogMinute);
    } else {
      scratch.drawIcon(scene.icon, 0, 0, scene.color, scene.elapsedMs,
                       scene.iconLevel);
    }
    for (uint8_t y = 0; y < 8; ++y) {
      for (uint8_t x = 0; x < 8; ++x) {
        iconPixels[y * 8 + x] = scratch.getPixel(x, y);
      }
    }
    hasIconPixels = true;
  }

  jsonDocument_.clear();
  JsonDocument& document = jsonDocument_;
  document["ok"] = true;
  document["client_render"] = true;
  document["key"] = scene.key;
  document["elapsed_ms"] = scene.elapsedMs;
  document["step_ms"] = scene.stepMs;
  document["hold_ms"] = scene.holdMs;
  document["base_duration_ms"] = scene.baseDurationMs;
  document["duration_ms"] = scene.durationMs;
  document["page_count"] = scene.pageCount;
  document["text_width"] = scene.textWidth;
  document["style"] = animationStyleName(scene.style);
  document["text"] = scene.text;
  document["previous_text"] = scene.previousText;
  document["color"] = (static_cast<uint32_t>(scene.color.r) << 16) |
                      (static_cast<uint32_t>(scene.color.g) << 8) |
                      scene.color.b;
  document["clock_font"] = scene.clockFont;
  document["icon_label"] = scene.iconLabel;
  document["custom_icon_id"] = scene.customIconId;
  document["linked_icon"] = scene.linkedIcon;
  document["app_elapsed_ms"] = appEngine_.appElapsedMs(nowMs);
  document["app"] = appEngine_.currentAppName();
  document["paused"] = appEngine_.paused();

  JsonArray glyphs = document.createNestedArray("glyphs");
  uint32_t seen[96]{};
  uint8_t seenCount = 0;
  size_t offset = 0;
  const String glyphText = scene.text + scene.previousText;
  while (offset < glyphText.length() && seenCount < 96) {
    const uint32_t codepoint = nextCodepoint(glyphText, offset);
    bool duplicate = false;
    for (uint8_t index = 0; index < seenCount; ++index) {
      if (seen[index] == codepoint) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    seen[seenCount++] = codepoint;
    const Glyph glyph =
        scene.clockFont ? clockGlyphFor(codepoint) : glyphFor(codepoint);
    JsonObject encoded = glyphs.createNestedObject();
    encoded["c"] = codepoint;
    encoded["w"] = glyph.width;
    JsonArray rows = encoded.createNestedArray("r");
    for (uint8_t row : glyph.rows) rows.add(row);
  }
  if (!scene.iconLabel.isEmpty()) {
    offset = 0;
    while (offset < scene.iconLabel.length() && seenCount < 96) {
      const uint32_t codepoint = nextCodepoint(scene.iconLabel, offset);
      bool duplicate = false;
      for (uint8_t index = 0; index < seenCount; ++index) {
        if (seen[index] == codepoint) duplicate = true;
      }
      if (duplicate) continue;
      seen[seenCount++] = codepoint;
      const Glyph glyph = glyphFor(codepoint);
      JsonObject encoded = glyphs.createNestedObject();
      encoded["c"] = codepoint;
      encoded["w"] = glyph.width;
      JsonArray rows = encoded.createNestedArray("r");
      for (uint8_t row : glyph.rows) rows.add(row);
    }
  }
  if (scene.style == AnimationStyle::Flip) {
    // The browser synthesizes split-flap transition letters. Supply their
    // glyphs even when none occur in the final text; otherwise those frames
    // briefly turn blank while the physical display continues animating.
    for (uint32_t codepoint = '?'; codepoint <= 'Z'; ++codepoint) {
      if (codepoint != '?' && (codepoint < 'A' || codepoint > 'Z')) continue;
      bool duplicate = false;
      for (uint8_t index = 0; index < seenCount; ++index) {
        if (seen[index] == codepoint) {
          duplicate = true;
          break;
        }
      }
      if (duplicate || seenCount >= 96) continue;
      seen[seenCount++] = codepoint;
      const Glyph glyph =
          scene.clockFont ? clockGlyphFor(codepoint) : glyphFor(codepoint);
      JsonObject encoded = glyphs.createNestedObject();
      encoded["c"] = codepoint;
      encoded["w"] = glyph.width;
      JsonArray rows = encoded.createNestedArray("r");
      for (uint8_t row : glyph.rows) rows.add(row);
    }
  }
  JsonArray icon = document.createNestedArray("icon");
  if (hasIconPixels) {
    for (const CRGB& pixel : iconPixels) {
      icon.add((static_cast<uint32_t>(pixel.r) << 16) |
               (static_cast<uint32_t>(pixel.g) << 8) | pixel.b);
    }
  }
  if (scene.sparklineCount >= 2) {
    JsonArray values = document.createNestedArray("sparkline");
    for (uint8_t index = 0; index < scene.sparklineCount; ++index) {
      values.add(scene.sparkline[index]);
    }
    document["sparkline_marker"] = scene.sparklineMarker;
    document["sparkline_blink"] = scene.sparklineBlinkMarker;
    document["sparkline_bands"] = scene.sparklineBandColors;
    document["sparkline_low"] = scene.sparklineLowThreshold;
    document["sparkline_high"] = scene.sparklineHighThreshold;
  }
  if (document.overflowed()) {
    jsonDocument_.clear();
    server_.send(507, "application/json",
                 "{\"ok\":false,\"error\":\"scene too complex\"}");
    return;
  }
  sendJson(200, document);
  networkHoldUntilMs_ = millis() + 750;
}

void WebPortal::handleIconAnimation() {
  recordWebBreadcrumb();
  if (dataServices_.busy()) {
    server_.send(503, "application/json",
                 "{\"ok\":false,\"error\":\"radio busy\"}");
    return;
  }
  const uint32_t iconId =
      strtoul(server_.arg("id").c_str(), nullptr, 10);
  CRGB pixels[64]{};
  uint16_t delayMs = 0;
  uint8_t frameCount = 0;
  if (iconId == 0 ||
      !iconStore_.copyFrameByIndex(iconId, 0, pixels, delayMs, frameCount) ||
      frameCount == 0) {
    server_.send(404, "application/json",
                 "{\"ok\":false,\"error\":\"icon unavailable\"}");
    return;
  }

  constexpr size_t kFrameBytes = 2 + 64 * 3;
  server_.sendHeader("Cache-Control", "public, max-age=86400");
  server_.sendHeader("Connection", "close");
  server_.setContentLength(2 + static_cast<size_t>(frameCount) * kFrameBytes);
  server_.send(200, "application/octet-stream", "");
  const uint8_t header[2] = {1, frameCount};
  server_.sendContent(reinterpret_cast<const char*>(header), sizeof(header));
  uint8_t packed[kFrameBytes];
  for (uint8_t frame = 0; frame < frameCount; ++frame) {
    uint8_t actualCount = 0;
    if (!iconStore_.copyFrameByIndex(iconId, frame, pixels, delayMs,
                                     actualCount)) {
      break;
    }
    packed[0] = delayMs >> 8;
    packed[1] = delayMs & 0xff;
    for (uint8_t index = 0; index < 64; ++index) {
      packed[2 + index * 3] = pixels[index].r;
      packed[3 + index * 3] = pixels[index].g;
      packed[4 + index * 3] = pixels[index].b;
    }
    server_.sendContent(reinterpret_cast<const char*>(packed), sizeof(packed));
  }
  networkHoldUntilMs_ = millis() + 250;
}

void WebPortal::handleGetConfig() {
  recordWebBreadcrumb();
  if (!sendCachedBrowserConfig()) {
    server_.send(503, "application/json",
                 "{\"ok\":false,\"error\":\"settings temporarily busy\"}");
    return;
  }
  networkHoldUntilMs_ = millis() + 3000;
}

void WebPortal::handleBrightnessPreview() {
  recordWebBreadcrumb();
  if (!checkAdminSession()) return;
  if (dataServices_.busy() || iconStore_.downloading()) {
    server_.send(503, "application/json",
                 "{\"ok\":false,\"error\":\"data refresh in progress\"}");
    return;
  }
  DynamicJsonDocument requested(768);
  const DeserializationError parseError =
      deserializeJson(requested, server_.arg("plain"));
  if (parseError || requested.overflowed()) {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }

  DynamicJsonDocument filtered(512);
  JsonObject preview = filtered.to<JsonObject>();
  constexpr const char* kKeys[] = {
      "auto_brightness", "manual_brightness", "minimum_brightness",
      "maximum_brightness", "ldr_dark", "ldr_bright", "brightness_gamma"};
  for (const char* key : kKeys) {
    if (!requested[key].isNull()) {
      preview[key] = requested[key];
    }
  }
  if (preview.size() == 0) {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"no brightness settings\"}");
    return;
  }

  String error;
  if (!configStore_.updateFromJson(preview, error)) {
    DynamicJsonDocument response(256);
    response["ok"] = false;
    response["error"] =
        error.isEmpty() ? "invalid brightness settings" : error;
    sendJson(400, response);
    return;
  }
  const AppConfig config = configStore_.snapshot();
  DynamicJsonDocument response(256);
  response["ok"] = true;
  response["brightness"] = peripherals_.desiredBrightness(config);
  response["auto_brightness"] = config.autoBrightness;
  response["manual_brightness"] = config.manualBrightness;
  response["saved"] = false;
  sendJson(200, response);
}

void WebPortal::handleSaveConfig() {
  recordWebBreadcrumb();
  if (!checkAdminSession()) return;
  if (dataServices_.busy() || iconStore_.downloading()) {
    server_.send(503, "application/json",
                 "{\"ok\":false,\"error\":\"settings temporarily busy\"}");
    return;
  }
  ScopedTlsHeapLease settingsHeap;
  const std::unique_ptr<AppConfig> before(
      new AppConfig(configStore_.snapshot()));
  String error;
  {
    // Parse in-place so the strings remain in the request body instead of
    // being duplicated into ArduinoJson's pool.  The fixed document lives in
    // WebPortal's BSS and cannot fail due to fragmented runtime heap.
    String requestBody = server_.arg("plain");
    jsonDocument_.clear();
    const DeserializationError parseError =
        deserializeJson(jsonDocument_, requestBody.begin(),
                        requestBody.length());
    if (parseError || jsonDocument_.overflowed()) {
      Serial.printf("[web] Settings JSON rejected: %s, %u bytes%s\n",
                    parseError.c_str(),
                    static_cast<unsigned>(requestBody.length()),
                    jsonDocument_.overflowed() ? ", overflow" : "");
      jsonDocument_.clear();
      server_.send(400, "application/json",
                   "{\"ok\":false,\"error\":\"invalid JSON\"}");
      return;
    }
    const JsonObjectConst requested =
        jsonDocument_.as<JsonObjectConst>();
    if (!before->wifiSsid.isEmpty() && requested.containsKey("wifi_ssid") &&
        requested["wifi_ssid"].as<String>().isEmpty()) {
      server_.send(
          400, "application/json",
          "{\"ok\":false,\"error\":\"wifi_ssid cannot be cleared remotely\"}");
      return;
    }
    std::unique_ptr<AppConfig> candidate(new AppConfig());
    if (!configStore_.updateFromJson(requested, error, candidate.get(),
                                     false)) {
      DynamicJsonDocument response(512);
      response["ok"] = false;
      response["error"] =
          error.isEmpty() ? "invalid settings" : error;
      sendJson(400, response);
      return;
    }
    jsonDocument_.clear();
    if (!configStore_.save(*candidate)) {
      DynamicJsonDocument response(512);
      response["ok"] = false;
      response["error"] = "could not save settings";
      sendJson(507, response);
      return;
    }
  }
  const std::unique_ptr<AppConfig> after(
      new AppConfig(configStore_.snapshot()));
  iconStore_.reconcileCache(*after);
  iconStore_.requestSyncScan();
  if (!cacheBrowserConfig()) {
    Serial.println(F("[web] Could not refresh browser config cache"));
  }
  const bool restartRequired =
      before->wifiSsid != after->wifiSsid ||
      before->wifiPassword != after->wifiPassword ||
      before->deviceName != after->deviceName ||
      before->timezone != after->timezone ||
      before->orientation180 != after->orientation180;
  DynamicJsonDocument response(128);
  response["ok"] = true;
  response["restart"] = restartRequired;
  sendJson(200, response);
  if (restartRequired) {
    restartPending_ = true;
    restartAtMs_ = millis() + 900;
  }
}

void WebPortal::handleNotify() {
  recordWebBreadcrumb();
  if (!checkApiKeyOrAdminSession()) {
    return;
  }
  DynamicJsonDocument document(4096);
  if (deserializeJson(document, server_.arg("plain"))) {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  const String text = document["text"] | String("");
  if (text.isEmpty()) {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"text is required\"}");
    return;
  }
  const uint32_t color = parseHtmlColor(document["color"] | "#ffffff",
                                        0xffffff);
  const uint16_t duration =
      constrain(document["duration_ms"] | 8000, 500, 60000);
  const String animation = document["animation"] | String("ticker");
  appEngine_.notify(text, color, duration, animation);
  if (document["beep"] | false) {
    const AppConfig config = configStore_.snapshot();
    buzzer_.playConfigured(config.notificationTone, config);
  }
  server_.send(200, "application/json", "{\"ok\":true}");
}

void WebPortal::handleFrame() {
  recordWebBreadcrumb();
  if (!checkApiKeyOrAdminSession()) {
    return;
  }
  DynamicJsonDocument document(12288);
  if (deserializeJson(document, server_.arg("plain"))) {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  JsonArrayConst pixels = document["pixels"].as<JsonArrayConst>();
  if (!remoteDisplay_.setJsonFrame(pixels, server_.client().remoteIP(),
                                   configStore_.snapshot())) {
    server_.send(
        400, "application/json",
        "{\"ok\":false,\"error\":\"pixels must contain 256 RGB integers\"}");
    return;
  }
  server_.send(200, "application/json", "{\"ok\":true}");
}

void WebPortal::handleButton() {
  recordWebBreadcrumb();
  if (!checkAdminSession()) return;
  DynamicJsonDocument document(256);
  if (deserializeJson(document, server_.arg("plain"))) {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  String button = document["button"] | String("");
  button.toLowerCase();
  const AppConfig config = configStore_.snapshot();
  String action;
  if (button == "left") {
    action = config.buttonShortLeft;
  } else if (button == "middle" || button == "centre" ||
             button == "center") {
    action = config.buttonShortMiddle;
  } else if (button == "right") {
    action = config.buttonShortRight;
  } else if (button == "long_left") {
    action = config.buttonLongLeft;
  } else if (button == "long_middle" || button == "long_centre" ||
             button == "long_center") {
    action = config.buttonLongMiddle;
  } else if (button == "long_right") {
    action = config.buttonLongRight;
  } else if (button == "previous" || button == "next" ||
             button == "auto" || button == "refresh" ||
             button == "device_info" || button == "counter_toggle" ||
             button == "counter_reset" || button == "none") {
    // Semantic commands remain available for API clients. Browser button IDs
    // above are logical (left stays left at 180 degrees) and use the persisted
    // mapping exactly like physical controls.
    action = button;
  } else {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"unknown button\"}");
    return;
  }

  bool applied = true;
  if (action == "previous") {
    appEngine_.previous(config);
  } else if (action == "auto") {
    // Mirror the physical centre button: Counter owns it while selected;
    // elsewhere it advances once and resumes the automatic slideshow.
    if (!appEngine_.toggleCounter(config)) {
      appEngine_.resumeAutomaticAndAdvance(config);
    }
  } else if (action == "next") {
    appEngine_.next(config);
  } else if (action == "refresh") {
    Serial.println(F("[input] browser mapped refresh"));
    appEngine_.showRefreshAnimation();
  } else if (action == "device_info") {
    if (!appEngine_.resetCounter(config)) {
      const String address =
          network_.connected()
              ? network_.ipAddress()
              : network_.accessPointName() + " PASS " +
                    network_.accessPointPassword();
      appEngine_.notify(address, 0x65ffb6, 9000, "ticker");
    }
  } else if (action == "counter_toggle") {
    applied = appEngine_.controlCounter("toggle", config);
  } else if (action == "counter_reset") {
    applied = appEngine_.controlCounter("reset", config);
  } else if (action == "none") {
    applied = false;
  } else {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"invalid button mapping\"}");
    return;
  }
  if (applied) buzzer_.playConfigured(config.buttonTone, config);
  buttonWakeRequested_ = true;
  StaticJsonDocument<128> response;
  response["ok"] = true;
  response["action"] = action;
  response["applied"] = applied;
  sendJson(200, response);
}

void WebPortal::handleCounter() {
  recordWebBreadcrumb();
  if (!checkAdminSession()) return;
  StaticJsonDocument<192> request;
  if (deserializeJson(request, server_.arg("plain"))) {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"invalid JSON\"}");
    return;
  }
  String action = request["action"] | String("toggle");
  action.toLowerCase();
  const bool known = action == "open" || action == "show" ||
                     action == "start" || action == "pause" ||
                     action == "toggle" || action == "reset";
  if (!known) {
    server_.send(400, "application/json",
                 "{\"ok\":false,\"error\":\"unknown counter action\"}");
    return;
  }
  const AppConfig config = configStore_.snapshot();
  if (!appEngine_.controlCounter(action, config)) {
    server_.send(
        409, "application/json",
        "{\"ok\":false,\"error\":\"enable Counter and finish Sync first\"}");
    return;
  }
  buzzer_.playConfigured(config.buttonTone, config);
  buttonWakeRequested_ = true;
  const CounterStatus status = appEngine_.counterStatus();
  StaticJsonDocument<256> response;
  response["ok"] = true;
  response["counter_count_down"] = status.countDown;
  response["counter_running"] = status.running;
  response["counter_finished"] = status.finished;
  response["counter_elapsed_seconds"] = status.elapsedSeconds;
  response["counter_remaining_seconds"] = status.remainingSeconds;
  sendJson(200, response);
}

void WebPortal::handleFirmwareUpload() {
  HTTPUpload& upload = server_.upload();
  if (upload.status == UPLOAD_FILE_START) {
    firmwareAuthorized_ = hasAdminSession();
    if (!firmwareAuthorized_) {
      updateFailed_ = true;
      return;
    }
    const String app = appEngine_.currentAppName();
    recordCrashBreadcrumb(app.c_str(),
                          dataServices_.syncTelemetry().activeProvider,
                          CrashContext::FirmwareUpdate, millis());
    updateFailed_ = false;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
      updateFailed_ = true;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (firmwareAuthorized_ && !updateFailed_ &&
        Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      updateFailed_ = true;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (firmwareAuthorized_ && !updateFailed_ && !Update.end(true)) {
      updateFailed_ = true;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (firmwareAuthorized_) Update.abort();
    updateFailed_ = true;
  }
}

}  // namespace pixelos
