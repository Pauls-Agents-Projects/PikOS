#pragma once

#include <Arduino.h>

#include <vector>

#include "Config.h"
#include "GarminActivityUtils.h"

namespace pixelos {

struct Headline {
  String source;
  String sourceUrl;
  String title;
  uint32_t id = 0;
  uint32_t iconId = 0;
  uint32_t publishedAt = 0;
};

struct NetatmoReading {
  bool available = false;
  String moduleName;
  float temperatureC = NAN;
  float humidityPercent = NAN;
  float pressureHpa = NAN;
  float co2Ppm = NAN;
  uint32_t measurementTime = 0;
  // Optional station modules. Netatmo returns these alongside the selected
  // outdoor module, so keep one coherent station snapshot instead of making
  // separate network requests for rain and wind.
  bool rainAvailable = false;
  float rainRateMmHour = NAN;
  float rainLastHourMm = NAN;
  float rainLast24HoursMm = NAN;
  uint32_t rainMeasurementTime = 0;
  bool windAvailable = false;
  float windSpeedKph = NAN;
  float windGustKph = NAN;
  int16_t windDirectionDegrees = -1;
  int16_t windGustDirectionDegrees = -1;
  uint32_t windMeasurementTime = 0;
  float windMaximumKph = NAN;
  int16_t windMaximumDirectionDegrees = -1;
  uint32_t windMaximumTime = 0;
  String error;
};

struct MarketQuote {
  bool available = false;
  String symbol;
  String name;
  String currency;
  float price = NAN;
  float percentChange = NAN;
  bool marketOpen = false;
  float sparkline[23]{};
  uint8_t sparklineCount = 0;
  String error;
};

enum class EnergyBand : uint8_t { Low = 0, Average = 1, High = 2 };

struct EnergyPricePoint {
  uint32_t timestamp = 0;
  float centsKwh = NAN;
  EnergyPricePoint() = default;
  EnergyPricePoint(uint32_t pointTimestamp, float pointCentsKwh)
      : timestamp(pointTimestamp), centsKwh(pointCentsKwh) {}
};

// Elering currently publishes quarter-hour prices. Two 25-hour DST days need
// 200 entries; retain a small margin while keeping the long-lived cache
// strictly bounded.
constexpr size_t kMaximumEnergyPricePoints = 208;

struct EnergyPriceReading {
  bool available = false;
  float currentCentsKwh = NAN;
  float averageCentsKwh = NAN;
  float lowThresholdCentsKwh = NAN;
  float highThresholdCentsKwh = NAN;
  EnergyBand band = EnergyBand::Average;
  int8_t nextDirection = 0;
  uint32_t nextChangeAt = 0;
  uint16_t pointsToday = 0;
  float sparkline[23]{};
  uint8_t sparklineCount = 0;
  int8_t currentSparklineIndex = -1;
  uint32_t chartStartsAt = 0;
  uint32_t chartEndsAt = 0;
  String error;
};

struct CalendarEvent {
  String source;
  String title;
  uint32_t startsAt = 0;
  uint32_t endsAt = 0;
  uint32_t alertAt = 0;
  uint32_t id = 0;
  uint32_t iconId = 0;
  bool allDay = false;
};

// Keep enough departures to let a pinned Travel app advance locally between
// central syncs. The 32x8 presentation still shows only the next two.
constexpr uint8_t kMaximumTravelDepartures = 4;
// The default five-loop central-sync cadence is several minutes. Ten minutes
// keeps predictions live through a normal rotation while still making a
// missed/offline refresh visibly fall back to the scheduled timetable.
constexpr uint32_t kTravelPredictionStaleSeconds = 600;

struct TravelDeparture {
  String route;
  String destination;
  uint32_t expectedAt = 0;
  uint32_t scheduledAt = 0;
  bool realtime = false;
};

struct TravelStopSnapshot {
  String label;
  String stopId;
  String line;
  uint32_t iconId = 0;
  uint32_t fetchedAt = 0;
  TravelDeparture departures[kMaximumTravelDepartures];
  uint8_t departureCount = 0;
  String error;
};

// Compact, last-good Garmin record. It is replaced only after all bounded
// read-only requests parse successfully; transient failures merely mark the
// existing values stale so the display never performs network work itself.
struct ActivitySnapshot {
  bool valid = false;
  bool stale = true;
  bool reconnectRequired = false;
  uint32_t updatedAt = 0;
  uint32_t latestStartedAt = 0;
  uint32_t stepsToday = 0;
  uint32_t distanceTodayM = 0;
  uint32_t latestDistanceM = 0;
  uint32_t latestDurationSec = 0;
  uint16_t latestCaloriesKcal = 0;
  GarminActivityType latestType = GarminActivityType::Other;
  String latestName;
  String error;
};

const char* garminActivityTypeName(GarminActivityType type);

struct OnlineData {
  std::vector<Headline> headlines;
  NetatmoReading netatmo;
  std::vector<MarketQuote> marketQuotes;
  EnergyPriceReading energy;
  std::vector<EnergyPricePoint> energyPoints;
  String energyChartRange = "today";
  std::vector<CalendarEvent> calendarEvents;
  std::vector<TravelStopSnapshot> travelStops;
  std::vector<Headline> redditPosts;
  ActivitySnapshot activity;
  uint32_t rssUpdatedAt = 0;
  uint32_t netatmoUpdatedAt = 0;
  uint32_t marketUpdatedAt = 0;
  uint32_t energyUpdatedAt = 0;
  uint32_t calendarUpdatedAt = 0;
  uint32_t travelUpdatedAt = 0;
  uint32_t redditUpdatedAt = 0;
  uint32_t activityUpdatedAt = 0;
  String rssError;
  String marketError;
  String calendarError;
  String travelError;
  String redditError;
  String activityError;
};

// Compact telemetry copy for the settings page. It intentionally excludes
// headline/event/quote vectors so a five-second status poll cannot allocate
// and duplicate every app's cached content on the display task.
struct OnlineStatus {
  size_t headlineCount = 0;
  size_t marketCount = 0;
  size_t calendarCount = 0;
  size_t travelCount = 0;
  size_t redditCount = 0;
  bool activityConnected = false;
  ActivitySnapshot activity;
  uint8_t marketChartPoints = 0;
  NetatmoReading netatmo;
  EnergyPriceReading energy;
  uint32_t rssUpdatedAt = 0;
  uint32_t netatmoUpdatedAt = 0;
  uint32_t marketUpdatedAt = 0;
  uint32_t energyUpdatedAt = 0;
  uint32_t calendarUpdatedAt = 0;
  uint32_t travelUpdatedAt = 0;
  uint32_t redditUpdatedAt = 0;
  uint32_t activityUpdatedAt = 0;
  String rssError;
  String marketError;
  String calendarError;
  String travelError;
  String redditError;
  String activityError;
  bool travelStale = true;
};

enum class OnlineProvider : uint8_t {
  None = 0,
  Rss,
  Netatmo,
  Market,
  Energy,
  Calendar,
  Travel,
  Reddit,
  Activity
};

struct SyncTelemetry {
  OnlineProvider activeProvider = OnlineProvider::None;
  uint32_t cyclesStarted = 0;
  uint32_t cyclesCompleted = 0;
  uint32_t cyclesAborted = 0;
  uint32_t providerJobs = 0;
  uint32_t providerFailures = 0;
  uint32_t currentSyncElapsedMs = 0;
  uint32_t currentProviderElapsedMs = 0;
  uint32_t lastSyncDurationMs = 0;
  uint32_t averageSyncDurationMs = 0;
  uint32_t lastProviderDurationMs = 0;
  uint32_t maximumProviderDurationMs = 0;
  uint8_t quarantinedProviderMask = 0;
};

struct DataCacheTelemetry {
  uint16_t headlines = 0;
  uint16_t marketQuotes = 0;
  uint16_t energyPoints = 0;
  uint16_t calendarEvents = 0;
  uint16_t travelStops = 0;
  uint16_t redditPosts = 0;
  uint16_t activityRecords = 0;
  uint32_t rssBytes = 0;
  uint32_t climateBytes = 0;
  uint32_t marketBytes = 0;
  uint32_t energyBytes = 0;
  uint32_t calendarBytes = 0;
  uint32_t travelBytes = 0;
  uint32_t redditBytes = 0;
  uint32_t activityBytes = 0;
  // Vector storage plus the bytes currently occupied by cached String data.
  // Arduino String does not expose its allocation capacity, so this is a
  // stable lower-bound estimate intended for leak/regression monitoring.
  uint32_t estimatedBytes = 0;
};

class DataServices {
 public:
  explicit DataServices(ConfigStore& configStore);
  void begin();
  // Cheap timestamp-derived work is separate from provider service so main()
  // can call it even while browser/icon radio work prevents a network start.
  // An internal limiter makes repeated calls inexpensive.
  void tickLocal(uint32_t nowMs);
  void tick(bool wifiConnected, uint32_t nowMs, bool allowProviderStart);
  OnlineStatus statusSnapshot() const;
  NetatmoReading netatmoSnapshot() const;
  EnergyPriceReading energySnapshot() const;
  ActivitySnapshot activitySnapshot() const;
  bool headlineAt(size_t index, Headline& value, size_t& count,
                  String& error) const;
  bool headlineById(uint32_t id, Headline& value) const;
  bool marketAt(size_t index, MarketQuote& value, size_t& count,
                String& error) const;
  bool calendarAt(size_t index, CalendarEvent& value, size_t& count,
                  String& error) const;
  // Copies one configured stop and derives its live view from cached absolute
  // epochs. No network access occurs here; expired departures are omitted and
  // stale predictions fall back to their scheduled epochs.
  bool travelAt(size_t index, TravelStopSnapshot& value, size_t& count,
                String& error, bool& stale) const;
  bool redditAt(size_t index, Headline& value, size_t& count,
                String& error) const;
  bool consumeNewHeadlineEvent(uint32_t& headlineId);
  bool consumeRainStartEvent(NetatmoReading& reading);
  bool consumeEnergyBandEvent();
  bool consumeCalendarAlertEvent(CalendarEvent& event);
  bool requestRefresh();
  // A provider that panicked on the previous boot, or repeatedly failed on
  // this boot, is removed from the central queue. Cached data and every other
  // app stay usable; a deliberate reboot gives the provider one clean retry.
  void quarantineProvider(OnlineProvider provider, const char* reason);
  bool busy() const { return taskRunning_; }
  bool refreshInProgress() const {
    // A future scheduling deadline is idle time, not active data transfer.
    // Keeping it out of this signal prevents the sync animation and compact
    // status response from lingering between/after provider jobs.
    return syncActive_ || taskRunning_ || forceRefreshMask_ != 0;
  }
  uint8_t syncCompleted() const { return syncCompleted_; }
  uint8_t syncTotal() const { return syncTotal_; }
  // Time-weighted progress advances within a provider rather than jumping
  // only when a whole HTTPS job completes. Permille keeps enough resolution
  // for all 32 physical pixels without floating-point work in the renderer.
  uint16_t syncProgressPermille() const;
  uint8_t syncProgressPercent() const {
    return static_cast<uint8_t>(
        min<uint16_t>(100, (syncProgressPermille() + 5U) / 10U));
  }
  SyncTelemetry syncTelemetry() const;
  DataCacheTelemetry cacheTelemetry() const;

 private:
  static void taskEntry(void* argument);
  bool runFetch(const AppConfig& config, bool fetchRss, bool fetchNetatmo,
                bool fetchMarket, bool fetchEnergy, bool fetchCalendar,
                bool fetchTravel, bool fetchReddit, bool fetchActivity);
  bool fetchRss(const AppConfig& config, std::vector<Headline>& headlines,
                String& error);
  bool fetchRssUrl(const String& url, const String& source,
                   uint8_t maximumItems, uint16_t maximumChars,
                   std::vector<Headline>& headlines, String& error,
                   const std::vector<IconItem>* dynamicSources = nullptr);
  bool fetchNetatmo(AppConfig& config, NetatmoReading& reading);
  bool refreshNetatmoToken(AppConfig& config, String& error);
  bool fetchMarket(const AppConfig& config,
                   std::vector<MarketQuote>& quotes, String& error);
  bool fetchEnergy(const AppConfig& config, EnergyPriceReading& reading,
                   std::vector<EnergyPricePoint>& points);
  bool fetchCalendar(const AppConfig& config,
                     std::vector<CalendarEvent>& events, String& error);
  bool fetchCalendarUrl(const String& url, const String& source,
                        uint32_t iconId, const AppConfig& config,
                        std::vector<CalendarEvent>& events, String& error);
  bool fetchTravel(const AppConfig& config,
                   std::vector<TravelStopSnapshot>& stops, String& error);
  bool fetchReddit(const AppConfig& config,
                   std::vector<Headline>& posts, String& error);
  bool fetchGarminActivity(AppConfig& config, ActivitySnapshot& activity);
  bool refreshGarminToken(AppConfig& config, String& error,
                          bool& reconnectRequired);
  bool fetchGarminJson(AppConfig& config, const String& path,
                       String& payload, String& error,
                       bool& reconnectRequired,
                       bool& refreshedAfterAuthorizationError);

  ConfigStore& configStore_;
  mutable SemaphoreHandle_t mutex_ = nullptr;
  OnlineData data_;
  volatile bool taskRunning_ = false;
  // Calendar and market parsing leave less than 2 KiB at the historical
  // high-water mark. Reddit's Atom parser adds several String temporaries, so
  // retain explicit headroom rather than letting a large provider response
  // turn into an unrelated-looking String fault.
  static constexpr uint32_t kWorkerStackBytes = 14336;
  alignas(StackType_t) uint8_t workerStack_[kWorkerStackBytes]{};
  StaticTask_t workerTaskBuffer_{};
  TaskHandle_t workerTask_ = nullptr;
  volatile bool pendingRss_ = false;
  volatile bool pendingNetatmo_ = false;
  volatile bool pendingMarket_ = false;
  volatile bool pendingEnergy_ = false;
  volatile bool pendingCalendar_ = false;
  volatile bool pendingTravel_ = false;
  volatile bool pendingReddit_ = false;
  volatile bool pendingActivity_ = false;
  // One bit per online provider. Keeping the bits until their individual jobs
  // start makes "refresh all" a real queue instead of forcing only the first
  // provider selected on that loop pass.
  volatile uint8_t forceRefreshMask_ = 0;
  // Leave a short main-loop service window between HTTPS providers so queued
  // browser button/status requests are handled before the next socket opens.
  volatile uint32_t nextJobEarliestMs_ = 0;
  volatile uint32_t syncRequestedAtMs_ = 0;
  volatile uint32_t offlineSinceMs_ = 0;
  volatile bool syncActive_ = false;
  volatile uint8_t syncCompleted_ = 0;
  volatile uint8_t syncTotal_ = 0;
  volatile uint8_t syncStartedProviderMask_ = 0;
  volatile OnlineProvider activeProvider_ = OnlineProvider::None;
  volatile uint32_t syncStartedAtMs_ = 0;
  volatile uint32_t providerStartedAtMs_ = 0;
  volatile uint32_t syncCyclesStarted_ = 0;
  volatile uint32_t syncCyclesCompleted_ = 0;
  volatile uint32_t syncCyclesAborted_ = 0;
  volatile uint32_t providerJobs_ = 0;
  volatile uint32_t providerFailures_ = 0;
  volatile uint32_t lastSyncDurationMs_ = 0;
  volatile uint32_t averageSyncDurationMs_ = 0;
  volatile uint32_t lastProviderDurationMs_ = 0;
  volatile uint32_t maximumProviderDurationMs_ = 0;
  volatile uint8_t quarantinedProviderMask_ = 0;
  uint8_t consecutiveProviderFailures_[8]{};
  // EWMA duration per OnlineProvider (Rss..Activity). Zero means use the
  // conservative built-in estimate until that provider has completed once.
  volatile uint32_t providerAverageDurationMs_[8]{};
  volatile uint32_t syncProviderEstimateMs_[8]{};
  // Short-lived Garmin bearer state never enters AppConfig, SPIFFS, NVS,
  // logs or browser JSON. The resolved profile name is non-secret and cached
  // only until reboot unless the user supplied garmin_display_name.
  String garminAccessToken_;
  String garminResolvedDisplayName_;
  uint32_t lastDerivedCheckMs_ = 0;
  volatile bool newHeadlineEvent_ = false;
  volatile bool rainStartEvent_ = false;
  volatile bool energyBandEvent_ = false;
  volatile bool calendarAlertEvent_ = false;
  static constexpr uint8_t kRecentHeadlineCapacity = 64;
  uint32_t recentHeadlineIds_[kRecentHeadlineCapacity]{};
  uint8_t recentHeadlineCount_ = 0;
  uint8_t recentHeadlineCursor_ = 0;
  uint32_t pendingHeadlineId_ = 0;
  bool previousRainStateKnown_ = false;
  bool previousRaining_ = false;
  uint32_t lastRainEventMeasurementTime_ = 0;
  NetatmoReading pendingRainReading_;
  int8_t previousEnergyBand_ = -1;
  uint32_t previousCalendarAlertId_ = 0;
  CalendarEvent pendingCalendarAlert_;
};

}  // namespace pixelos
