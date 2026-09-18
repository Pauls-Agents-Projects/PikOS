#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <vector>

namespace pixelos {

struct IconItem {
  String value;
  uint32_t iconId = 0;
  IconItem() = default;
  IconItem(const String& itemValue, uint32_t itemIconId)
      : value(itemValue), iconId(itemIconId) {}
};

struct CalendarItem {
  String name;
  String url;
  uint32_t iconId = 0;
  CalendarItem() = default;
  CalendarItem(const String& itemName, const String& itemUrl,
               uint32_t itemIconId)
      : name(itemName), url(itemUrl), iconId(itemIconId) {}
};

// A deliberately small Tallinn live-departure subscription. Stop identifiers
// come from the public transport SIRI endpoint; an empty line accepts every
// route serving that stop. The compact tab-delimited representation keeps the
// settings document bounded without shipping a city-wide stop index.
struct TravelStopItem {
  String label;
  String stopId;
  String line;
  uint32_t iconId = 0;
  TravelStopItem() = default;
  TravelStopItem(const String& itemLabel, const String& itemStopId,
                 const String& itemLine, uint32_t itemIconId)
      : label(itemLabel),
        stopId(itemStopId),
        line(itemLine),
        iconId(itemIconId) {}
};

constexpr uint8_t kMaximumTravelStopItems = 16;

struct AppConfig {
  uint16_t schemaVersion = 28;

  String deviceName = "Ulanzi PikOS";
  String language = "en";
  String timezone = "EET-2EEST,M3.5.0/3,M10.5.0/4";
  bool orientation180 = false;
  bool clock24Hour = true;
  String clockTimezone2 = "utc";
  String clockTimezone3 = "new_york";
  String clockTimezone4 = "tokyo";
  uint8_t clockTimezoneSeconds = 4;
  // One "timezone<TAB>LaMetric icon ID" record per line.
  String clockTimezoneItems;
  // Up to twelve lines of:
  // HH:MM<TAB>weekday mask (Mon=1 ... Sun=64)<TAB>label<TAB>tone<TAB>enabled
  // Empty label/tone fields use localized "Alarm" and alarmTone.
  String alarmItems;

  String wifiSsid;
  String wifiPassword;

  bool autoBrightness = true;
  uint8_t manualBrightness = 48;
  // Very small FastLED global scale values quantize individual RGB channels
  // differently. A floor of 10 is still dim in a dark room but preserves hue.
  uint8_t minimumBrightness = 10;
  uint8_t maximumBrightness = 255;
  uint16_t ldrDark = 300;
  uint16_t ldrBright = 850;
  float brightnessGamma = 1.8f;
  // TC001 battery ADC endpoints vary noticeably between units. These values
  // are intentionally user-calibratable; 640 is the observed full reading on
  // this clock rather than AWTRIX's overly optimistic fixed 665.
  // PikOS learns/logs the useful ADC range automatically. These fields remain
  // on disk for backwards compatibility, but are no longer exposed as manual
  // controls in the browser. 565 is the observed shutdown neighbourhood on
  // this TC001 and avoids reporting nearly half a battery at power-off.
  uint16_t batteryEmptyRaw = 565;
  uint16_t batteryFullRaw = 640;
  bool batterySaverEnabled = true;
  uint8_t batterySaverThreshold = 15;
  uint8_t batterySaverBrightness = 18;
  // When enabled, Battery joins the slideshow only at or below the same low
  // threshold used by the brightness saver. `batteryEnabled` remains the
  // explicit always-show choice; no unreliable USB/charging guess is used.
  bool batteryAutoLowEnabled = false;

  bool timeEnabled = true;
  bool climateEnabled = true;
  bool netatmoEnabled = false;
  // "local" uses the TC001 sensor; "netatmo" uses the selected station
  // module. netatmoEnabled remains only for migrating pre-11 settings.
  String climateProvider = "local";
  bool rssEnabled = true;
  bool batteryEnabled = false;
  bool marketEnabled = false;
  bool energyEnabled = true;
  bool calendarEnabled = false;
  bool travelEnabled = false;
  bool redditEnabled = false;
  // Garmin Connect is an opt-in, read-only provider. Authentication itself
  // happens once in the desktop helper; the clock only refreshes a provisioned
  // DI token during the explicit central Sync app.
  bool activityEnabled = false;
  bool counterEnabled = false;
  String timeAnimation = "static";
  String climateAnimation = "flip";
  String netatmoAnimation = "ticker";
  String rssAnimation = "ticker";
  String batteryAnimation = "static";
  String marketAnimation = "ticker";
  String energyAnimation = "ticker";
  String calendarAnimation = "ticker";
  String travelAnimation = "ticker";
  String redditAnimation = "ticker";
  String activityAnimation = "ticker";
  uint16_t appDurationSeconds = 10;
  uint16_t maximumAppSeconds = 45;
  uint16_t scrollStepMs = 42;
  uint16_t maximumMessageChars = 160;
  // All network-backed widgets share one refresh cadence and one serialized
  // provider queue. The queue is now an explicit slideshow app and is inserted
  // after this many complete local-app rounds. The provider-specific fields
  // below remain only for downgrade compatibility.
  uint8_t syncEveryLoops = 5;
  uint32_t timeColor = 0xFFFFFF;
  uint32_t climateColor = 0x42D9FF;
  uint32_t netatmoColor = 0x65FF78;
  uint32_t rssColor = 0xFFB347;
  uint32_t batteryColor = 0xA78BFA;
  uint32_t calendarColor = 0x5AA9FF;
  uint32_t travelColor = 0x64D2FF;
  uint32_t redditColor = 0xFF6314;
  uint32_t activityColor = 0x65FFB6;

  // LaMetric gallery icon IDs. Zero keeps the built-in PikOS fallback.
  // Zero selects PikOS's live analogue face. Gallery icon 82 remains valid
  // when explicitly selected or migrated from an existing configuration.
  uint32_t timeIconId = 0;
  uint32_t climateIconId = 1257;
  uint32_t netatmoIconId = 73;
  uint32_t rssIconId1 = 0;
  uint32_t rssIconId2 = 0;
  uint32_t marketIconId = 47;
  uint32_t batteryIconId = 389;
  uint32_t batteryIconHighId = 389;
  uint32_t batteryIconMediumId = 0;
  uint32_t batteryIconLowId = 0;
  uint32_t climateIconColdId = 0;
  uint32_t climateIconHotId = 0;
  uint32_t netatmoIconColdId = 0;
  uint32_t netatmoIconHotId = 0;
  uint32_t netatmoIconSunnyId = 0;
  uint32_t netatmoIconRainId = 0;
  uint32_t climateTemperatureLowIconId = 0;
  uint32_t climateTemperatureMediumIconId = 1257;
  uint32_t climateTemperatureHighIconId = 0;
  uint32_t climateHumidityLowIconId = 0;
  uint32_t climateHumidityMediumIconId = 1257;
  uint32_t climateHumidityHighIconId = 0;
  uint32_t climateWindIconId = 2298;
  uint32_t climateRainIconId = 72;
  uint32_t netatmoTemperatureLowIconId = 0;
  uint32_t netatmoTemperatureMediumIconId = 73;
  uint32_t netatmoTemperatureHighIconId = 0;
  uint32_t netatmoHumidityLowIconId = 0;
  uint32_t netatmoHumidityMediumIconId = 73;
  uint32_t netatmoHumidityHighIconId = 0;
  uint32_t marketIconUpId = 0;
  uint32_t marketIconDownId = 0;
  uint32_t energyIconLowId = 60725;
  uint32_t energyIconAverageId = 60726;
  uint32_t energyIconHighId = 69668;
  uint32_t rssFallbackIconId = 1278;
  uint32_t calendarIconId = 0;
  uint32_t redditIconId = 33701;
  uint32_t activityIconId = 397;
  uint32_t counterIconId = 35196;

  // Select which cached Garmin values take turns in the Activity app. At
  // least one is restored by normalize() if a malformed settings document
  // disables every row.
  bool activityShowSteps = true;
  bool activityShowDailyDistance = true;
  bool activityShowLatestDistance = true;
  bool activityShowLatestDuration = false;
  bool activityShowLatestCalories = false;

  // Counter state is runtime-only, while its mode and starting duration are
  // durable preferences. Stopwatch counts up; timer counts down.
  String counterMode = "down";
  uint32_t counterDurationSeconds = 300;

  String rssUrl1 = "https://www.err.ee/rss";
  String rssUrl2 = "https://www.postimees.ee/rss/";
  // One "feed URL<TAB>LaMetric icon ID" record per line.
  String rssFeedItems;
  uint8_t rssHeadlineCount = 8;
  uint16_t rssRefreshMinutes = 10;
  bool rssBeepOnNew = false;

  String netatmoClientId;
  String netatmoClientSecret;
  String netatmoAccessToken;
  String netatmoRefreshToken;
  String netatmoDeviceId;
  String netatmoModuleId;
  String netatmoModuleName;
  uint16_t netatmoRefreshMinutes = 10;
  bool climateRainAlertEnabled = true;

  String marketProvider = "yahoo";
  String marketApiKey;
  String marketSymbols = "AAPL,EUR/USD";
  // Optional comma-separated overrides such as "AAPL=1234,EUR/USD=5678".
  String marketTickerIcons;
  // One "ticker<TAB>LaMetric icon ID" record per line.
  String marketItems;
  uint16_t marketRefreshMinutes = 20;
  bool marketChartEnabled = true;
  String marketChartRange = "5d";

  String energyCountry = "EE";
  uint16_t energyRefreshMinutes = 15;
  bool energyIncludeVat = true;
  float energyVatPercent = 24.0f;
  bool energyBeepOnChange = false;
  bool energyChartEnabled = true;
  String energyChartRange = "today";


  // Repeatable named iCal feeds. The in-memory records contain
  // "name<TAB>URL<TAB>icon ID". URLs are persisted separately in the NVS
  // credential vault and represented by opaque slot tokens in browser JSON.
  String calendarItems;
  // Legacy fixed slots retained for migration from schema 14 and older.
  String calendarName1 = "Calendar";
  String calendarIcalUrl;
  String calendarName2;
  String calendarIcalUrl2;
  String calendarName3;
  String calendarIcalUrl3;
  String calendarName4;
  String calendarIcalUrl4;
  uint16_t calendarRefreshMinutes = 15;
  uint8_t calendarMaximumEvents = 8;
  bool calendarAlertsEnabled = true;
  String calendarTone = "soft";

  // One "label<TAB>SIRI stop ID<TAB>optional line<TAB>icon ID" record per
  // line. Network access is keyless and happens only during the central Sync
  // app; the live countdown is derived from the resulting absolute epochs.
  String travelStopItems;

  String redditSubreddits = "eesti,technology";
  // One "subreddit<TAB>LaMetric icon ID" record per line.
  String redditItems;
  String redditPeriod = "week";
  uint8_t redditPostCount = 5;
  uint16_t redditRefreshMinutes = 15;

  // Unofficial Garmin DI OAuth credentials generated locally by
  // tools/pikos_credentials.py. The client ID and refresh token are removed
  // from the SPIFFS settings document and stored together in a checksummed
  // NVS blob. Access tokens are deliberately runtime-only in DataServices.
  String garminDomain = "garmin.com";
  String garminClientId;
  String garminRefreshToken;
  // Optional non-secret cache. Leaving it blank costs one small profile call
  // after boot; the provider then keeps the resolved name in RAM.
  String garminDisplayName;

  bool sleepEnabled = false;
  String sleepStart = "23:00";
  String sleepEnd = "07:00";
  bool sleepIdleEnabled = false;
  uint16_t sleepIdleMinutes = 30;
  String sleepDisplayMode = "off";
  uint16_t buttonWakeMinutes = 5;
  // Stable action identifiers used by both the physical switches and the
  // browser remote.  Keeping the mapping in configuration lets orientation
  // and interaction preferences change without hard-coding another UI path.
  String buttonShortLeft = "previous";
  String buttonShortMiddle = "auto";
  String buttonShortRight = "next";
  String buttonLongLeft = "refresh";
  String buttonLongMiddle = "device_info";
  String buttonLongRight = "none";
  bool remoteWakesDisplay = true;
  bool hourlyChimeEnabled = false;
  uint8_t hourlyChimeStart = 8;
  uint8_t hourlyChimeEnd = 22;
  String newsTone = "news";
  String climateTone = "soft";
  String alarmTone = "alert";
  String counterTone = "alert";
  String hourlyTone = "hourly";
  String notificationTone = "double";
  String buttonTone = "click";
  String energyTone = "soft";
  // Comma-delimited built-in tone IDs hidden from the user's sound library.
  // The melodies remain in flash for downgrade safety; playConfigured()
  // treats disabled IDs as silent even if a stale reference survives.
  String disabledBuiltinTones;
  uint8_t buzzerVolume = 18;
  uint8_t buzzerPitchPercent = 99;
  uint16_t buzzerHighNoteBoostPercent = 250;
  String customRtttl1;
  String customRtttl2;
  String customRtttl3;
  String customRtttl4;
  String customRtttl5;
  String customRtttl6;

  bool remoteEnabled = false;
  uint16_t remoteTimeoutMs = 1800;
  String remoteSourceIp;
  String apiKey;
};

class ConfigStore {
 public:
  bool begin();
  AppConfig snapshot() const;
  void copyTo(AppConfig& destination) const;
  uint32_t revision() const { return revision_; }
  const AppConfig& currentUnsafe() const { return config_; }
  bool save(AppConfig& config);
  bool updateFromJson(JsonObjectConst object, String& error,
                      AppConfig* result = nullptr, bool publish = true);
  void toJson(JsonObject object, bool includeSecrets) const;
  String browserJson() const;
  void updateNetatmoTokens(const String& accessToken,
                           const String& refreshToken);
  bool updateGarminCredentials(const String& domain,
                               const String& clientId,
                               const String& refreshToken);
  void factoryReset();

 private:
  void loadDefaults();
  void loadSecretVault();
  bool saveSecretVault(const AppConfig& config);
  bool deserialize(const String& json);
  static void normalize(AppConfig& config);
  static String generateApiKey();

  mutable SemaphoreHandle_t mutex_ = nullptr;
  Preferences preferences_;
  AppConfig config_;
  volatile uint32_t revision_ = 1;
  bool fileSystemReady_ = false;
  bool migrationPending_ = false;
};

uint32_t parseHtmlColor(const String& value, uint32_t fallback);
String htmlColor(uint32_t color);
std::vector<IconItem> parseIconItems(const String& encoded,
                                    uint8_t maximumItems = 8);
uint32_t iconForItem(const String& encoded, const String& value);
std::vector<CalendarItem> parseCalendarItems(const String& encoded,
                                             uint8_t maximumItems = 16);
std::vector<TravelStopItem> parseTravelStopItems(
    const String& encoded,
    uint8_t maximumItems = kMaximumTravelStopItems);

}  // namespace pixelos
