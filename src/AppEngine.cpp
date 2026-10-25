#include "AppEngine.h"

#include <time.h>

#include <algorithm>

#include "CounterTime.h"
#include "PixelFont.h"

namespace pixelos {
namespace {

class ScopedAppEngineLock {
 public:
  explicit ScopedAppEngineLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    if (mutex_ != nullptr) xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
  }
  ~ScopedAppEngineLock() {
    if (mutex_ != nullptr) xSemaphoreGiveRecursive(mutex_);
  }

 private:
  SemaphoreHandle_t mutex_;
};

CRGB rgb(uint32_t value) {
  return CRGB((value >> 16) & 0xff, (value >> 8) & 0xff, value & 0xff);
}

int parseMinutes(const String& value) {
  if (value.length() != 5 || value[2] != ':') {
    return -1;
  }
  const int hour = value.substring(0, 2).toInt();
  const int minute = value.substring(3, 5).toInt();
  if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return -1;
  }
  return hour * 60 + minute;
}

uint32_t temperatureColor(float temperatureC) {
  if (isnan(temperatureC)) {
    return 0xb0b8c8;
  }
  // Five bands based on Estonia's 1991–2020 climate normals distribute the
  // thermometer across useful outdoor weather instead of spending most of
  // the range on rare extremes.
  if (temperatureC < -5.0f) {
    return 0x006dff;
  }
  if (temperatureC < 2.0f) {
    return 0x00bfff;
  }
  if (temperatureC < 10.0f) {
    return 0x00ff72;
  }
  if (temperatureC < 18.0f) {
    return 0xffa000;
  }
  return 0xff2400;
}

uint8_t temperatureIconLevel(float temperatureC) {
  if (isnan(temperatureC)) return 0;
  if (temperatureC < -5.0f) return 20;
  if (temperatureC < 2.0f) return 40;
  if (temperatureC < 10.0f) return 60;
  if (temperatureC < 18.0f) return 80;
  return 100;
}

uint32_t windColor(float speedKph) {
  if (isnan(speedKph)) return 0xb0b8c8;
  if (speedKph < 20.0f) return 0x42d9ff;
  if (speedKph < 40.0f) return 0x00ff72;
  if (speedKph < 65.0f) return 0xffa000;
  return 0xff2400;
}

uint32_t humidityColor(float humidityPercent) {
  if (isnan(humidityPercent)) {
    return 0xb0b8c8;
  }
  if (humidityPercent < 35.0f) {
    return 0xffb347;
  }
  if (humidityPercent <= 65.0f) {
    return 0x42d9ff;
  }
  return 0x3977ff;
}

bool supportedToneName(const String& value) {
  return value == "off" || value == "click" || value == "double" ||
         value == "news" || value == "rise" || value == "soft" ||
         value == "alert" || value == "gran_vals" || value == "hourly" ||
         value == "custom1" || value == "custom2" || value == "custom3" ||
         value == "custom4" || value == "custom5" || value == "custom6";
}

String formattedCounterTime(uint64_t totalSeconds) {
  // With an 8x8 hourglass at the left, H:MM/MM:SS is the largest live value
  // that remains legible in the 23-pixel text lane. Above 99 hours the visual
  // saturates, while the internal stopwatch continues counting correctly.
  totalSeconds = min<uint64_t>(totalSeconds, 359999ULL);
  char value[8];
  if (totalSeconds >= 3600) {
    snprintf(value, sizeof(value), "%llu:%02llu",
             totalSeconds / 3600, (totalSeconds / 60) % 60);
  } else {
    snprintf(value, sizeof(value), "%llu:%02llu",
             totalSeconds / 60, totalSeconds % 60);
  }
  return String(value);
}

uint32_t counterSceneKey(bool countDown, bool running,
                         uint64_t shownSeconds) {
  return 0xc0000000UL |
         (countDown ? 0x20000000UL : 0) |
         (running ? 0x10000000UL : 0) |
         (static_cast<uint32_t>(shownSeconds) & 0x0fffffffUL);
}

String eventCountdown(time_t startsAt, time_t now, const String& language) {
  const int64_t remaining = static_cast<int64_t>(startsAt) - now;
  if (remaining <= 0) {
    return language == "et" ? String("PRAEGU") : String("NOW");
  }

  // Round upward so an event never briefly reads 0M before it begins. Two
  // units are enough to be useful on a 24-pixel text field without turning a
  // nearby event into another long ticker.
  const uint32_t totalMinutes =
      static_cast<uint32_t>((remaining + 59) / 60);
  const uint32_t days = totalMinutes / (24U * 60U);
  const uint32_t hours = (totalMinutes / 60U) % 24U;
  const uint32_t minutes = totalMinutes % 60U;
  const char* dayUnit = language == "et" ? "P" : "D";
  const char* hourUnit = language == "et" ? "T" : "H";
  if (days != 0) {
    String value = String(days) + dayUnit;
    if (hours != 0) value += " " + String(hours) + hourUnit;
    return value;
  }
  if (hours != 0) {
    String value = String(hours) + hourUnit;
    if (minutes != 0) value += " " + String(minutes) + "M";
    return value;
  }
  return String(minutes) + "M";
}

String localizedCalendarError(String error, const String& language) {
  // Provider errors carry a source label for the settings status pane. The
  // app already has a calendar icon, so repeating "Calendar: Calendar…" on
  // eight rows wastes nearly the whole first screen.
  while (error.startsWith("Calendar: ")) {
    error.remove(0, strlen("Calendar: "));
  }
  while (error.startsWith("Kalender: ")) {
    error.remove(0, strlen("Kalender: "));
  }
  if (language != "et") return error;
  error.replace("Calendar has no upcoming events",
                "Tulevasi sündmusi pole");
  error.replace("Calendar iCal URL is not configured",
                "Kalendri iCal-aadress puudub");
  error.replace("Calendar iCal URL failed",
                "Kalendri iCal-aadress ebaõnnestus");
  error.replace("Calendar HTTP", "Kalender HTTP");
  return error;
}

String localizedTravelError(String error, const String& language) {
  while (error.startsWith("Travel: ")) error.remove(0, strlen("Travel: "));
  if (language != "et") return error;
  error.replace("No upcoming departures", "Väljumisi pole");
  error.replace("No matching departures", "Sobivaid väljumisi pole");
  error.replace("Stop not returned", "Peatuse andmed puuduvad");
  error.replace("Unknown stop ID; use Tallinn stop search",
                "Tundmatu peatus; kasuta Tallinna peatuseotsingut");
  error.replace("Some stop IDs were not found",
                "Mõnda peatust ei leitud");
  error.replace("Travel has no saved departures", "Peatusi pole lisatud");
  error.replace("Travel is not configured", "Peatusi pole lisatud");
  error.replace("Travel HTTP", "Ühistransport HTTP");
  return error;
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097LL + dayOfEra - 719468LL;
}

time_t utcDate(int year, unsigned month, unsigned day, unsigned hour) {
  return static_cast<time_t>(
      daysFromCivil(year, month, day) * 86400LL + hour * 3600LL);
}

bool leapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

uint8_t daysInMonth(int year, uint8_t month) {
  static const uint8_t days[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month == 2 && leapYear(year) ? 29 : days[month - 1];
}

uint8_t weekday(int year, uint8_t month, uint8_t day) {
  int value = static_cast<int>((daysFromCivil(year, month, day) + 4) % 7);
  return static_cast<uint8_t>(value < 0 ? value + 7 : value);
}

uint8_t nthSunday(int year, uint8_t month, uint8_t nth) {
  return static_cast<uint8_t>(
      1 + (7 - weekday(year, month, 1)) % 7 + (nth - 1) * 7);
}

uint8_t lastSunday(int year, uint8_t month) {
  const uint8_t last = daysInMonth(year, month);
  return static_cast<uint8_t>(
      last - weekday(year, month, last));
}

bool europeanDst(time_t epoch, int year) {
  const time_t start = utcDate(year, 3, lastSunday(year, 3), 1);
  const time_t end = utcDate(year, 10, lastSunday(year, 10), 1);
  return epoch >= start && epoch < end;
}

bool americanDst(time_t epoch, int year, int16_t standardOffsetMinutes) {
  const time_t start =
      utcDate(year, 3, nthSunday(year, 3, 2), 2) -
      standardOffsetMinutes * 60;
  const time_t end =
      utcDate(year, 11, nthSunday(year, 11, 1), 2) -
      (standardOffsetMinutes + 60) * 60;
  return epoch >= start && epoch < end;
}

bool australianDst(time_t epoch, int year,
                   int16_t standardOffsetMinutes) {
  const time_t end =
      utcDate(year, 4, nthSunday(year, 4, 1), 3) -
      (standardOffsetMinutes + 60) * 60;
  const time_t start =
      utcDate(year, 10, nthSunday(year, 10, 1), 2) -
      standardOffsetMinutes * 60;
  return epoch < end || epoch >= start;
}

bool newZealandDst(time_t epoch, int year) {
  const time_t end =
      utcDate(year, 4, nthSunday(year, 4, 1), 3) - 780 * 60;
  const time_t start =
      utcDate(year, 9, lastSunday(year, 9), 2) - 720 * 60;
  return epoch < end || epoch >= start;
}

struct ClockFace {
  String key;
  int16_t offsetMinutes;
  uint32_t color;
  bool local;
  uint32_t iconId;
};

ClockFace clockFace(const AppConfig& config, time_t epoch,
                    uint32_t cycleElapsedMs,
                    int16_t forcedFaceIndex = -1) {
  // This runs on every display frame. Count the compact records in-place and
  // decode only the selected one; constructing a vector here used to churn
  // the heap precisely while provider TLS handshakes needed it most.
  uint8_t faceCount = 0;
  size_t cursor = 0;
  while (cursor <= config.clockTimezoneItems.length() && faceCount < 8) {
    int end = config.clockTimezoneItems.indexOf('\n', cursor);
    if (end < 0) end = config.clockTimezoneItems.length();
    if (static_cast<size_t>(end) > cursor) ++faceCount;
    if (static_cast<size_t>(end) >= config.clockTimezoneItems.length()) break;
    cursor = static_cast<size_t>(end) + 1;
  }
  if (faceCount == 0) faceCount = 1;
  const size_t faceIndex =
      forcedFaceIndex >= 0
          ? static_cast<size_t>(forcedFaceIndex) % faceCount
          : (cycleElapsedMs /
             (static_cast<uint32_t>(
                  max<uint8_t>(2, config.clockTimezoneSeconds)) *
              1000UL)) %
                faceCount;
  String key = "local";
  uint32_t selectedIconId = config.timeIconId;
  if (!config.clockTimezoneItems.isEmpty()) {
    cursor = 0;
    uint8_t recordIndex = 0;
    while (cursor <= config.clockTimezoneItems.length()) {
      int end = config.clockTimezoneItems.indexOf('\n', cursor);
      if (end < 0) end = config.clockTimezoneItems.length();
      if (static_cast<size_t>(end) > cursor) {
        if (recordIndex == faceIndex) {
          String record =
              config.clockTimezoneItems.substring(cursor, end);
          record.replace("\r", "");
          const int separator = record.lastIndexOf('\t');
          key = separator >= 0 ? record.substring(0, separator) : record;
          key.trim();
          if (separator >= 0) {
            String id = record.substring(separator + 1);
            id.trim();
            selectedIconId = strtoul(id.c_str(), nullptr, 10);
          }
          break;
        }
        ++recordIndex;
      }
      if (static_cast<size_t>(end) >= config.clockTimezoneItems.length()) {
        break;
      }
      cursor = static_cast<size_t>(end) + 1;
    }
  }
  if (key == "local") {
    return {key, 0, 0xffffff, true, selectedIconId};
  }

  tm utc{};
  gmtime_r(&epoch, &utc);
  const int year = utc.tm_year + 1900;
  if (key == "utc") {
    return {key, 0, 0x70b7ff, false, selectedIconId};
  }
  if (key == "london" || key == "lisbon" || key == "tallinn" ||
      key == "helsinki" || key == "berlin" || key == "paris" ||
      key == "amsterdam") {
    const int16_t base =
        (key == "tallinn" || key == "helsinki")
            ? 120
            : ((key == "london" || key == "lisbon") ? 0 : 60);
    return {key,
            static_cast<int16_t>(
                base + (europeanDst(epoch, year) ? 60 : 0)),
            static_cast<uint32_t>(
                base == 120 ? 0x65ffb6
                            : (base == 0 ? 0xb9d7ff : 0xffd166)),
            false, selectedIconId};
  }
  if (key == "new_york" || key == "toronto" || key == "chicago" ||
      key == "denver" || key == "los_angeles" ||
      key == "vancouver") {
    const int16_t base =
        (key == "new_york" || key == "toronto") ? -300
        : key == "chicago" ? -360
        : key == "denver" ? -420
                          : -480;
    return {key,
            static_cast<int16_t>(
                base + (americanDst(epoch, year, base) ? 60 : 0)),
            0xff9f68, false, selectedIconId};
  }
  if (key == "adelaide" || key == "sydney") {
    const int16_t base = key == "adelaide" ? 570 : 600;
    return {key,
            static_cast<int16_t>(
                base + (australianDst(epoch, year, base) ? 60 : 0)),
            0xb794ff, false, selectedIconId};
  }
  if (key == "auckland") {
    return {key,
            static_cast<int16_t>(
                720 + (newZealandDst(epoch, year) ? 60 : 0)),
            0x8edcff, false, selectedIconId};
  }

  struct FixedZone {
    const char* key;
    int16_t minutes;
    uint32_t color;
  };
  static constexpr FixedZone kFixedZones[] = {
      {"phoenix", -420, 0xffa65a},
      {"honolulu", -600, 0x60d5ff},
      {"mexico_city", -360, 0x77e091},
      {"sao_paulo", -180, 0x63d9a0},
      {"buenos_aires", -180, 0x72cfff},
      {"johannesburg", 120, 0xffbf69},
      {"cairo", 120, 0xffd166},
      {"dubai", 240, 0xffcf5a},
      {"delhi", 330, 0xff8c68},
      {"mumbai", 330, 0xff8c68},
      {"kathmandu", 345, 0xff9f7a},
      {"bangkok", 420, 0xffd65c},
      {"singapore", 480, 0x56e0c5},
      {"hong_kong", 480, 0xff6f91},
      {"shanghai", 480, 0xff6f91},
      {"seoul", 540, 0xff79c6},
      {"tokyo", 540, 0xff79c6},
      {"perth", 480, 0xb794ff},
      {"brisbane", 600, 0xb794ff},
  };
  for (const FixedZone& zone : kFixedZones) {
    if (key == zone.key) {
      return {key, zone.minutes, zone.color, false, selectedIconId};
    }
  }
  return {"local", 0, 0xffffff, true, selectedIconId};
}

tm clockTime(const ClockFace& face, time_t epoch) {
  tm value{};
  if (face.local) {
    localtime_r(&epoch, &value);
  } else {
    const time_t shifted = epoch + face.offsetMinutes * 60;
    gmtime_r(&shifted, &value);
  }
  return value;
}

String formattedClockTime(const tm& value, bool clock24Hour) {
  int hour = value.tm_hour;
  if (!clock24Hour) {
    hour %= 12;
    if (hour == 0) {
      hour = 12;
    }
  }
  char output[8];
  snprintf(output, sizeof(output), "%02d:%02d", hour, value.tm_min);
  return String(output);
}

String tickerIconLabel(String symbol) {
  symbol.trim();
  symbol.toUpperCase();
  if (symbol.startsWith("EUR")) {
    return "€";
  }
  if (symbol.startsWith("USD")) {
    return "$";
  }
  if (symbol.startsWith("GBP")) {
    return "£";
  }
  if (symbol.isEmpty()) {
    return "";
  }
  size_t offset = 0;
  const uint32_t point = nextCodepoint(symbol, offset);
  String result;
  if (point < 0x80) {
    result += static_cast<char>(point);
  }
  return result;
}

String activityTypeLabel(GarminActivityType type, const String& language) {
  const bool estonian = language == "et";
  switch (type) {
    case GarminActivityType::Running:
      return estonian ? "Jooks" : "Run";
    case GarminActivityType::Cycling:
      return estonian ? "Ratas" : "Ride";
    case GarminActivityType::Walking:
      return estonian ? "Kõnd" : "Walk";
    case GarminActivityType::Swimming:
      return estonian ? "Ujumine" : "Swim";
    case GarminActivityType::Other:
    default:
      return estonian ? "Aktiivsus" : "Activity";
  }
}

String smartNumber(float value, const String& unit, const String& language,
                   uint8_t maximumDecimals = 2,
                   int16_t maximumWidth = 23) {
  if (isnan(value)) {
    return "—";
  }
  for (int8_t decimals = maximumDecimals; decimals >= 0; --decimals) {
    String number(value, decimals);
    if (language == "et") {
      number.replace(".", ",");
    }
    const String candidate = number + unit;
    if (measureText(candidate) <= maximumWidth) {
      return candidate;
    }
  }

  // Compact large non-currency values only after the ordinary number no
  // longer fits.
  const float magnitude = fabsf(value);
  float divisor = 1.0f;
  const char* suffix = "";
  if (magnitude >= 999500000000.0f) {
    divisor = 1000000000000.0f;
    suffix = "T";
  } else if (magnitude >= 999500000.0f) {
    divisor = 1000000000.0f;
    suffix = "B";
  } else if (magnitude >= 999500.0f) {
    divisor = 1000000.0f;
    suffix = "M";
  } else if (magnitude >= 999.5f) {
    divisor = 1000.0f;
    suffix = "K";
  }
  if (divisor > 1.0f) {
    const float compact = value / divisor;
    for (int8_t decimals = compact < 10.0f && compact > -10.0f ? 1 : 0;
         decimals >= 0; --decimals) {
      String number(compact, decimals);
      if (language == "et") {
        number.replace(".", ",");
      }
      const String candidate = number + suffix + unit;
      if (measureText(candidate) <= maximumWidth || decimals == 0) {
        return candidate;
      }
    }
  }
  return String(lroundf(value)) + unit;
}

String compactMetricNumber(float value, const char* fullUnit,
                           const char* compactUnit,
                           const char* emergencyUnit,
                           const String& language,
                           uint8_t maximumDecimals = 1,
                           int16_t maximumWidth = 23) {
  const char* units[] = {fullUnit, compactUnit, emergencyUnit, ""};
  for (const char* unit : units) {
    const String candidate =
        smartNumber(value, unit, language, maximumDecimals, maximumWidth);
    if (measureText(candidate) <= maximumWidth) return candidate;
  }
  // The unitless attempt uses smartNumber's K/M/B compaction and therefore
  // remains bounded for any finite provider value.
  return smartNumber(value, "", language, 0, maximumWidth);
}

String currencyUnit(const String& currency) {
  if (currency.equalsIgnoreCase("USD")) {
    return "$";
  }
  if (currency.equalsIgnoreCase("EUR")) {
    return "€";
  }
  if (currency.equalsIgnoreCase("GBP")) {
    return "£";
  }
  return currency;
}

String smartCurrency(float value, const String& currency,
                     const String& language, int16_t maximumWidth = 23) {
  if (isnan(value)) {
    return "—";
  }
  const String symbol = currencyUnit(currency);
  auto localized = [&language](float number, uint8_t decimals) {
    String text(number, static_cast<unsigned int>(decimals));
    if (language == "et") text.replace(".", ",");
    return text;
  };
  for (int8_t decimals = 2; decimals >= 0; --decimals) {
    const String candidate = symbol + localized(value, decimals);
    if (measureClockText(candidate) <= maximumWidth) return candidate;
  }

  const float magnitude = fabsf(value);
  float divisor = 1.0f;
  const char* suffix = "";
  if (magnitude >= 999500000000.0f) {
    divisor = 1000000000000.0f;
    suffix = "T";
  } else if (magnitude >= 999500000.0f) {
    divisor = 1000000000.0f;
    suffix = "B";
  } else if (magnitude >= 999500.0f) {
    divisor = 1000000.0f;
    suffix = "M";
  } else if (magnitude >= 999.5f) {
    divisor = 1000.0f;
    suffix = "K";
  }
  if (divisor > 1.0f) {
    const float compact = value / divisor;
    const int8_t firstDecimals = fabsf(compact) < 10.0f ? 1 : 0;
    for (int8_t decimals = firstDecimals; decimals >= 0; --decimals) {
      const String candidate =
          symbol + localized(compact, decimals) + suffix;
      if (measureClockText(candidate) <= maximumWidth || decimals == 0) {
        return candidate;
      }
    }
  }
  return symbol + String(lroundf(value));
}

}  // namespace

AppEngine::AppEngine(PixelDisplay& display, Peripherals& peripherals,
                     DataServices& dataServices, ConfigStore& configStore,
                     LaMetricIcons& iconStore)
    : display_(display),
      peripherals_(peripherals),
      dataServices_(dataServices),
      configStore_(configStore),
      iconStore_(iconStore) {}

void AppEngine::begin() {
  if (mutex_ == nullptr) mutex_ = xSemaphoreCreateRecursiveMutex();
  ScopedAppEngineLock lock(mutex_);
  animator_.setIconStore(&iconStore_);
  current_ = LocalApp::Time;
  currentStarted_ = false;
  loopStartedMs_ = millis();
}

void AppEngine::queueTimedAlert(TimedAlertKind kind, const String& text,
                                const String& tone) {
  if (timedAlertCount_ >= kTimedAlertQueueCapacity) {
    // The fixed queue is sized for every configured alarm plus one timer.
    // Keep the guard in case future producers are added without enlarging it.
    return;
  }
  TimedAlertEvent& event = timedAlertQueue_[timedAlertTail_];
  event.kind = kind;
  event.text = text;
  event.tone = tone;
  timedAlertTail_ = (timedAlertTail_ + 1) % kTimedAlertQueueCapacity;
  ++timedAlertCount_;
}

void AppEngine::refreshAlarmSchedule(const AppConfig& config) {
  if (cachedAlarmItems_ == config.alarmItems) return;

  AlarmSlot previous[kMaximumAlarms];
  const uint8_t previousCount = alarmCount_;
  memcpy(previous, alarms_, sizeof(previous));
  memset(alarms_, 0, sizeof(alarms_));
  alarmCount_ = 0;
  cachedAlarmItems_ = config.alarmItems;

  size_t start = 0;
  while (start <= cachedAlarmItems_.length() &&
         alarmCount_ < kMaximumAlarms) {
    int end = cachedAlarmItems_.indexOf('\n', start);
    if (end < 0) end = cachedAlarmItems_.length();
    String record = cachedAlarmItems_.substring(start, end);
    record.replace("\r", "");
    if (!record.isEmpty()) {
      String fields[5];
      size_t fieldStart = 0;
      for (uint8_t field = 0; field < 5; ++field) {
        int separator = record.indexOf('\t', fieldStart);
        if (separator < 0) separator = record.length();
        fields[field] = record.substring(fieldStart, separator);
        fields[field].trim();
        fieldStart = static_cast<size_t>(separator) < record.length()
                         ? separator + 1
                         : record.length();
      }
      const int minute = parseMinutes(fields[0]);
      const uint8_t weekdayMask =
          fields[1].isEmpty()
              ? 0x7f
              : static_cast<uint8_t>(fields[1].toInt()) & 0x7f;
      String enabled = fields[4];
      enabled.toLowerCase();
      const bool isEnabled = enabled.isEmpty() ||
                             (enabled != "0" && enabled != "false" &&
                              enabled != "off");
      if (minute >= 0 && weekdayMask != 0 && isEnabled) {
        AlarmSlot& slot = alarms_[alarmCount_++];
        slot.minuteOfDay = static_cast<uint16_t>(minute);
        slot.weekdayMask = weekdayMask;
        fields[2].toCharArray(slot.label, sizeof(slot.label));
        if (supportedToneName(fields[3])) {
          fields[3].toCharArray(slot.tone, sizeof(slot.tone));
        }
        slot.lastFiredDayKey = UINT32_MAX;
        // Preserve same-day de-duplication when an unrelated setting is
        // edited while an alarm minute is still active.
        for (uint8_t oldIndex = 0; oldIndex < previousCount; ++oldIndex) {
          const AlarmSlot& old = previous[oldIndex];
          if (old.minuteOfDay == slot.minuteOfDay &&
              old.weekdayMask == slot.weekdayMask &&
              strcmp(old.label, slot.label) == 0) {
            slot.lastFiredDayKey = old.lastFiredDayKey;
            break;
          }
        }
      }
    }
    if (static_cast<size_t>(end) >= cachedAlarmItems_.length()) break;
    start = static_cast<size_t>(end) + 1;
  }
}

uint64_t AppEngine::counterElapsedAt(uint32_t nowMs) const {
  return counterProjectedElapsed(counterElapsedMs_, counterRunning_,
                                 counterConfigInitialized_,
                                 counterLastUpdateMs_, nowMs);
}

void AppEngine::updateCounterState(uint32_t nowMs,
                                   const AppConfig& config) {
  const bool countDown = config.counterMode != "up";
  const uint32_t duration =
      constrain(config.counterDurationSeconds, 1UL, 359999UL);
  if (!counterConfigInitialized_ || countDown != counterCountDown_ ||
      duration != counterConfiguredDurationSeconds_) {
    counterConfigInitialized_ = true;
    counterCountDown_ = countDown;
    counterConfiguredDurationSeconds_ = duration;
    counterRunning_ = false;
    counterCompletionQueued_ = false;
    counterElapsedMs_ = 0;
    counterLastUpdateMs_ = nowMs;
  } else if (counterTimestampAtOrAfter(nowMs, counterLastUpdateMs_)) {
    const uint32_t delta = counterForwardDelta(nowMs, counterLastUpdateMs_);
    counterLastUpdateMs_ = nowMs;
    if (counterRunning_) counterElapsedMs_ += delta;
  }

  if (!config.counterEnabled) {
    counterRunning_ = false;
    return;
  }
  if (!counterCountDown_) return;
  const uint64_t durationMs =
      static_cast<uint64_t>(counterConfiguredDurationSeconds_) * 1000ULL;
  if (counterElapsedMs_ < durationMs) return;
  counterElapsedMs_ = durationMs;
  counterRunning_ = false;
  if (!counterCompletionQueued_) {
    queueTimedAlert(TimedAlertKind::Timer,
                    config.language == "et" ? "TAIMER" : "TIMER",
                    config.counterTone);
    counterCompletionQueued_ = true;
  }
}

void AppEngine::tickBackground(uint32_t nowMs, const AppConfig& config) {
  if (lastBackgroundCounterTickMs_ != 0 &&
      nowMs - lastBackgroundCounterTickMs_ < 10) {
    return;
  }
  lastBackgroundCounterTickMs_ = nowMs;
  ScopedAppEngineLock lock(mutex_);
  updateCounterState(nowMs, config);

  if (lastAlarmPollMs_ != 0 && nowMs - lastAlarmPollMs_ < 500) return;
  lastAlarmPollMs_ = nowMs;
  refreshAlarmSchedule(config);
  const time_t now = time(nullptr);
  if (now <= 1672531200) return;
  const uint32_t epochMinute = static_cast<uint32_t>(now / 60);
  if (epochMinute == lastAlarmEpochMinute_) return;
  lastAlarmEpochMinute_ = epochMinute;

  tm local{};
  localtime_r(&now, &local);
  const uint16_t minuteOfDay = local.tm_hour * 60 + local.tm_min;
  const uint8_t weekdayBit =
      local.tm_wday == 0 ? 0x40 : static_cast<uint8_t>(1U << (local.tm_wday - 1));
  const uint32_t dayKey =
      static_cast<uint32_t>(local.tm_year + 1900) * 512UL + local.tm_yday;
  for (uint8_t index = 0; index < alarmCount_; ++index) {
    AlarmSlot& alarm = alarms_[index];
    if (alarm.minuteOfDay != minuteOfDay ||
        (alarm.weekdayMask & weekdayBit) == 0 ||
        alarm.lastFiredDayKey == dayKey) {
      continue;
    }
    alarm.lastFiredDayKey = dayKey;
    const String label = alarm.label[0] != '\0'
                             ? String(alarm.label)
                             : (config.language == "et" ? "ÄRATUS" : "ALARM");
    const String tone =
        alarm.tone[0] != '\0' ? String(alarm.tone) : config.alarmTone;
    queueTimedAlert(TimedAlertKind::Alarm, label, tone);
  }
}

bool AppEngine::toggleCounter(const AppConfig& config) {
  ScopedAppEngineLock lock(mutex_);
  if (current_ != LocalApp::Counter || !config.counterEnabled) return false;
  const uint32_t nowMs = millis();
  updateCounterState(nowMs, config);
  const uint64_t durationMs =
      static_cast<uint64_t>(counterConfiguredDurationSeconds_) * 1000ULL;
  if (counterCountDown_ && counterElapsedMs_ >= durationMs) {
    counterElapsedMs_ = 0;
    counterCompletionQueued_ = false;
  }
  counterRunning_ = !counterRunning_;
  counterLastUpdateMs_ = nowMs;
  paused_ = true;
  currentStarted_ = true;
  appStartedMs_ = nowMs;
  return true;
}

bool AppEngine::resetCounter(const AppConfig& config) {
  ScopedAppEngineLock lock(mutex_);
  if (current_ != LocalApp::Counter || !config.counterEnabled) return false;
  const uint32_t nowMs = millis();
  updateCounterState(nowMs, config);
  counterElapsedMs_ = 0;
  counterRunning_ = false;
  counterCompletionQueued_ = false;
  counterLastUpdateMs_ = nowMs;
  paused_ = true;
  currentStarted_ = true;
  appStartedMs_ = nowMs;
  return true;
}

bool AppEngine::controlCounter(const String& requestedAction,
                               const AppConfig& config) {
  String action = requestedAction;
  action.toLowerCase();
  if (action == "show") action = "open";
  if (action != "open" && action != "start" && action != "pause" &&
      action != "toggle" && action != "reset") {
    return false;
  }

  ScopedAppEngineLock lock(mutex_);
  if (!config.counterEnabled || current_ == LocalApp::Sync) return false;
  const uint32_t nowMs = millis();
  if (current_ != LocalApp::Counter) {
    advanceClockResume(nowMs, config);
    current_ = LocalApp::Counter;
    loopEligibleForCadence_ = false;
  }
  paused_ = true;
  refreshAnimationUntilMs_ = 0;
  notificationUntilMs_ = 0;
  animator_.stop();
  updateCounterState(nowMs, config);

  const uint64_t durationMs =
      static_cast<uint64_t>(counterConfiguredDurationSeconds_) * 1000ULL;
  if (action == "reset") {
    counterElapsedMs_ = 0;
    counterRunning_ = false;
    counterCompletionQueued_ = false;
  } else if (action == "pause") {
    counterRunning_ = false;
  } else if (action == "start") {
    if (counterCountDown_ && counterElapsedMs_ >= durationMs) {
      counterElapsedMs_ = 0;
      counterCompletionQueued_ = false;
    }
    counterRunning_ = true;
  } else if (action == "toggle") {
    if (counterCountDown_ && counterElapsedMs_ >= durationMs) {
      counterElapsedMs_ = 0;
      counterCompletionQueued_ = false;
    }
    counterRunning_ = !counterRunning_;
  }
  counterLastUpdateMs_ = nowMs;
  appStartedMs_ = nowMs;
  currentStarted_ = true;
  return true;
}

CounterStatus AppEngine::counterStatus() const {
  ScopedAppEngineLock lock(mutex_);
  CounterStatus status;
  status.countDown = counterCountDown_;
  status.running = counterRunning_;
  status.durationSeconds = counterConfiguredDurationSeconds_;
  const uint64_t elapsedMs = counterElapsedAt(millis());
  status.elapsedSeconds = static_cast<uint32_t>(min<uint64_t>(
      UINT32_MAX, elapsedMs / 1000ULL));
  if (counterCountDown_) {
    const uint64_t durationMs =
        static_cast<uint64_t>(counterConfiguredDurationSeconds_) * 1000ULL;
    status.finished = elapsedMs >= durationMs;
    status.remainingSeconds =
        status.finished
            ? 0
            : static_cast<uint32_t>((durationMs - elapsedMs + 999ULL) /
                                    1000ULL);
  }
  return status;
}

bool AppEngine::consumeTimedAlert(TimedAlertEvent& event) {
  ScopedAppEngineLock lock(mutex_);
  const uint32_t nowMs = millis();
  if (timedAlertCount_ == 0 ||
      (timedAlertPresentationUntilMs_ != 0 &&
       static_cast<int32_t>(timedAlertPresentationUntilMs_ - nowMs) > 0)) {
    return false;
  }
  event = timedAlertQueue_[timedAlertHead_];
  timedAlertQueue_[timedAlertHead_].text = "";
  timedAlertQueue_[timedAlertHead_].tone = "";
  timedAlertHead_ = (timedAlertHead_ + 1) % kTimedAlertQueueCapacity;
  --timedAlertCount_;
  timedAlertPresentationUntilMs_ = nowMs + 30000UL;
  return true;
}

void AppEngine::showTimedAlert(const TimedAlertEvent& event) {
  ScopedAppEngineLock lock(mutex_);
  const AppConfig config = configStore_.snapshot();
  String text = event.text;
  if (text.length() > config.maximumMessageChars) {
    text.remove(config.maximumMessageChars);
  }
  const bool timer = event.kind == TimedAlertKind::Timer;
  notify(text, timer ? 0xffa000 : 0xff453a, 30000, "flip",
         timer ? TinyIcon::Hourglass : TinyIcon::Clock, timer ? 0 : 50,
         timer ? config.counterIconId : 0, false);
}

bool AppEngine::appEnabled(LocalApp app, const AppConfig& config) const {
  switch (app) {
    case LocalApp::Time:
      return config.timeEnabled;
    case LocalApp::Climate:
      return config.climateEnabled;
    case LocalApp::Rss:
      return config.rssEnabled;
    case LocalApp::Market:
      return config.marketEnabled;
    case LocalApp::Energy:
      return config.energyEnabled;
    case LocalApp::Battery:
      return config.batteryEnabled ||
             (config.batteryAutoLowEnabled &&
              peripherals_.readings().batteryPercent <=
                  config.batterySaverThreshold);
    case LocalApp::Calendar:
      return config.calendarEnabled;
    case LocalApp::Travel:
      return config.travelEnabled;
    case LocalApp::Activity:
      return config.activityEnabled;
    case LocalApp::Reddit:
      return config.redditEnabled;
    case LocalApp::Counter:
      return config.counterEnabled;
    case LocalApp::Sync:
      return false;
  }
  return false;
}

LocalApp AppEngine::adjacentEnabled(int direction,
                                    const AppConfig& config) const {
  int index = static_cast<int>(current_);
  constexpr int kAppCount = static_cast<int>(LocalApp::Sync);
  for (uint8_t attempt = 0; attempt < kAppCount; ++attempt) {
    index = (index + direction + kAppCount) % kAppCount;
    const LocalApp candidate = static_cast<LocalApp>(index);
    if (appEnabled(candidate, config)) {
      return candidate;
    }
  }
  return current_;
}

LocalApp AppEngine::firstEnabled(const AppConfig& config) const {
  constexpr int kUserAppCount = static_cast<int>(LocalApp::Sync);
  for (int index = 0; index < kUserAppCount; ++index) {
    const LocalApp candidate = static_cast<LocalApp>(index);
    if (appEnabled(candidate, config)) return candidate;
  }
  return LocalApp::Time;
}

bool AppEngine::hasOnlineProviders(const AppConfig& config) const {
  return config.rssEnabled || config.marketEnabled || config.energyEnabled ||
         config.calendarEnabled || config.redditEnabled ||
         (config.travelEnabled &&
          !parseTravelStopItems(config.travelStopItems).empty()) ||
         (config.activityEnabled && !config.garminClientId.isEmpty() &&
          !config.garminRefreshToken.isEmpty()) ||
         (config.climateEnabled && config.climateProvider == "netatmo");
}

bool AppEngine::wrapsForward(LocalApp from, LocalApp to) const {
  return static_cast<uint8_t>(to) <= static_cast<uint8_t>(from);
}

void AppEngine::beginSync(uint32_t nowMs, LocalApp returnApp,
                          bool automatic) {
  syncReturnApp_ = returnApp == LocalApp::Sync ? LocalApp::Time : returnApp;
  syncReturnPaused_ = paused_;
  syncAutomatic_ = automatic;
  // A forced update is a real completed cadence point too. Without resetting
  // these counters, a manual update near a loop boundary could be followed by
  // an unnecessary automatic update on the very next wrap.
  firstSyncPending_ = false;
  loopsSinceSync_ = 0;
  pendingNavigation_ = PendingNavigation::None;
  syncStartedAtMs_ = nowMs;
  syncQuietSinceMs_ = 0;
  refreshAnimationStartedMs_ = nowMs;
  refreshAnimationUntilMs_ = nowMs + 1200;
  current_ = LocalApp::Sync;
  currentStarted_ = true;
  iconStore_.requestSyncScan();
  dataServices_.requestRefresh();
}

void AppEngine::finishSync(uint32_t nowMs, const AppConfig& config) {
  const bool cleanAutomaticBoundary =
      syncAutomatic_ && pendingNavigation_ == PendingNavigation::None;
  current_ = syncReturnApp_;
  paused_ = syncAutomatic_ ? false : syncReturnPaused_;
  if (!appEnabled(current_, config)) current_ = firstEnabled(config);
  if (pendingNavigation_ == PendingNavigation::Previous) {
    current_ = adjacentEnabled(-1, config);
    paused_ = true;
  } else if (pendingNavigation_ == PendingNavigation::Next) {
    current_ = adjacentEnabled(1, config);
    paused_ = true;
  } else if (pendingNavigation_ == PendingNavigation::Automatic) {
    current_ = adjacentEnabled(1, config);
    paused_ = false;
  }
  pendingNavigation_ = PendingNavigation::None;
  refreshAnimationUntilMs_ = 0;
  syncQuietSinceMs_ = 0;
  syncStartedAtMs_ = 0;
  syncAutomatic_ = false;
  currentStarted_ = false;
  notificationUntilMs_ = 0;
  animator_.stop();
  loopStartedMs_ = nowMs;
  loopEligibleForCadence_ = cleanAutomaticBoundary;
}

void AppEngine::advanceAutomatic(uint32_t nowMs,
                                 const AppConfig& config) {
  const LocalApp next = adjacentEnabled(1, config);
  if (!wrapsForward(current_, next)) {
    current_ = next;
    currentStarted_ = false;
    return;
  }

  if (!loopEligibleForCadence_) {
    // The user may have browsed or pinned apps for hours. The first wrap only
    // establishes a clean automatic-loop boundary; it is not one of the N
    // complete loops requested between Sync apps.
    loopEligibleForCadence_ = true;
    current_ = next;
    currentStarted_ = false;
    loopStartedMs_ = nowMs;
    return;
  }

  if (loopStartedMs_ != 0) {
    lastLoopDurationMs_ = nowMs - loopStartedMs_;
    estimatedLoopDurationMs_ =
        estimatedLoopDurationMs_ == 0
            ? lastLoopDurationMs_
            : (estimatedLoopDurationMs_ * 3UL + lastLoopDurationMs_) / 4UL;
  }

  const bool online = hasOnlineProviders(config);
  bool syncDue = false;
  if (online && firstSyncPending_) {
    syncDue = true;
    firstSyncPending_ = false;
  } else if (online) {
    ++loopsSinceSync_;
    syncDue = loopsSinceSync_ >= config.syncEveryLoops;
  } else {
    firstSyncPending_ = false;
    loopsSinceSync_ = 0;
  }
  if (syncDue) {
    loopsSinceSync_ = 0;
    beginSync(nowMs, next, true);
    return;
  }
  current_ = next;
  currentStarted_ = false;
  loopStartedMs_ = nowMs;
}

void AppEngine::next() {
  const AppConfig config = configStore_.snapshot();
  next(config);
}

void AppEngine::next(const AppConfig& config) {
  ScopedAppEngineLock lock(mutex_);
  if (current_ == LocalApp::Sync) {
    pendingNavigation_ = PendingNavigation::Next;
    return;
  }
  advanceClockResume(millis(), config);
  current_ = adjacentEnabled(1, config);
  paused_ = true;
  loopEligibleForCadence_ = false;
  refreshAnimationUntilMs_ = 0;
  currentStarted_ = false;
}

void AppEngine::previous() {
  const AppConfig config = configStore_.snapshot();
  previous(config);
}

void AppEngine::previous(const AppConfig& config) {
  ScopedAppEngineLock lock(mutex_);
  if (current_ == LocalApp::Sync) {
    pendingNavigation_ = PendingNavigation::Previous;
    return;
  }
  advanceClockResume(millis(), config);
  current_ = adjacentEnabled(-1, config);
  paused_ = true;
  loopEligibleForCadence_ = false;
  refreshAnimationUntilMs_ = 0;
  currentStarted_ = false;
}

void AppEngine::resumeAutomaticAndAdvance() {
  const AppConfig config = configStore_.snapshot();
  resumeAutomaticAndAdvance(config);
}

void AppEngine::resumeAutomaticAndAdvance(const AppConfig& config) {
  ScopedAppEngineLock lock(mutex_);
  if (current_ == LocalApp::Sync) {
    pendingNavigation_ = PendingNavigation::Automatic;
    return;
  }
  const uint32_t nowMs = millis();
  advanceClockResume(nowMs, config);
  const LocalApp previous = current_;
  current_ = adjacentEnabled(1, config);
  paused_ = false;
  if (wrapsForward(previous, current_)) {
    // Resuming last→first (or a one-app rotation) itself establishes a clean
    // boundary. The following wrap is therefore a real complete loop.
    loopEligibleForCadence_ = true;
    loopStartedMs_ = nowMs;
  }
  refreshAnimationUntilMs_ = 0;
  currentStarted_ = false;
}

void AppEngine::showRefreshAnimation() {
  ScopedAppEngineLock lock(mutex_);
  if (current_ == LocalApp::Sync) return;
  beginSync(millis(), current_, false);
}

String AppEngine::animationFor(LocalApp app,
                               const AppConfig& config) const {
  switch (app) {
    case LocalApp::Time:
      return config.timeAnimation;
    case LocalApp::Climate:
      return config.climateAnimation;
    case LocalApp::Rss:
      return config.rssAnimation;
    case LocalApp::Market:
      return config.marketAnimation;
    case LocalApp::Energy:
      return config.energyAnimation;
    case LocalApp::Battery:
      return config.batteryAnimation;
    case LocalApp::Calendar:
      return config.calendarAnimation;
    case LocalApp::Travel:
      return config.travelAnimation;
    case LocalApp::Activity:
      return config.activityAnimation;
    case LocalApp::Reddit:
      return config.redditAnimation;
    case LocalApp::Counter:
      return "static";
    case LocalApp::Sync:
      return "static";
  }
  return "static";
}

uint32_t AppEngine::colorFor(LocalApp app) const {
  switch (app) {
    case LocalApp::Time: {
      return currentClockColor_;
    }
    case LocalApp::Climate:
      switch (currentClimateMetric_) {
        case ClimateMetric::Temperature:
          return temperatureColor(currentTemperatureC_);
        case ClimateMetric::Humidity:
          return humidityColor(currentWeatherHumidity_);
        case ClimateMetric::Wind:
          return windColor(currentWindSpeedKph_);
        case ClimateMetric::Rain:
          return 0x3977ff;
      }
      return 0x42d9ff;
    case LocalApp::Rss:
      return 0xffa000;
    case LocalApp::Market:
      if (isnan(currentMarketChange_)) {
        return 0x9aa8c7;
      }
      return currentMarketChange_ > 0.005f
                 ? 0x00ff66
                 : (currentMarketChange_ < -0.005f ? 0xff2400 : 0xffd000);
    case LocalApp::Energy:
      return currentEnergyBand_ == EnergyBand::Low
                 ? 0x00ff66
                 : (currentEnergyBand_ == EnergyBand::High ? 0xff2400
                                                           : 0xffd000);
    case LocalApp::Battery: {
      const uint8_t battery = peripherals_.readings().batteryPercent;
      return battery > 50 ? 0x00ff66
                          : (battery > 20 ? 0xffd000 : 0xff2400);
    }
    case LocalApp::Calendar:
      return 0x5aa9ff;
    case LocalApp::Travel:
      return currentTravelColor_;
    case LocalApp::Activity:
      return currentActivityColor_;
    case LocalApp::Reddit:
      return 0xff6314;
    case LocalApp::Counter:
      if (counterCountDown_ && counterCompletionQueued_) return 0xff2400;
      return counterRunning_ ? 0x65ffb6 : 0x9aa8c7;
    case LocalApp::Sync:
      return 0xff2400;
  }
  return 0xffffff;
}

TinyIcon AppEngine::iconFor(LocalApp app) const {
  switch (app) {
    case LocalApp::Time:
      return TinyIcon::Clock;
    case LocalApp::Climate:
      switch (currentClimateMetric_) {
        case ClimateMetric::Temperature:
          return TinyIcon::Thermometer;
        case ClimateMetric::Wind:
          return TinyIcon::Wind;
        case ClimateMetric::Rain:
          return TinyIcon::Rain;
        case ClimateMetric::Humidity:
          return TinyIcon::Weather;
      }
      return TinyIcon::Weather;
    case LocalApp::Rss:
      return TinyIcon::Rss;
    case LocalApp::Market:
      return TinyIcon::None;
    case LocalApp::Energy:
      // Energy uses only the selected LaMetric animation. There is no PikOS
      // three-colour PikOS fallback, so a failed download remains visibly
      // missing and can retry rather than silently showing the wrong artwork.
      return TinyIcon::None;
    case LocalApp::Battery:
      return TinyIcon::Battery;
    case LocalApp::Calendar:
      return TinyIcon::Calendar;
    case LocalApp::Travel:
      return TinyIcon::Transit;
    case LocalApp::Activity:
      return TinyIcon::Activity;
    case LocalApp::Reddit:
      return TinyIcon::Reddit;
    case LocalApp::Counter:
      return TinyIcon::Hourglass;
    case LocalApp::Sync:
      return TinyIcon::None;
  }
  return TinyIcon::None;
}

uint32_t AppEngine::customIconFor(LocalApp app,
                                  const AppConfig& config) const {
  switch (app) {
    case LocalApp::Time:
      return currentClockIconId_;
    case LocalApp::Climate:
      if (currentClimateMetric_ == ClimateMetric::Temperature) {
        // The generated five-level mercury column is more informative than
        // swapping among three unrelated gallery frames.
        return 0;
      }
      if (currentClimateMetric_ == ClimateMetric::Humidity) {
        if (!isnan(currentWeatherHumidity_) &&
            currentWeatherHumidity_ < 35.0f) {
          return config.climateHumidityLowIconId;
        }
        if (!isnan(currentWeatherHumidity_) &&
            currentWeatherHumidity_ > 65.0f) {
          return config.climateHumidityHighIconId;
        }
        return config.climateHumidityMediumIconId;
      }
      if (currentClimateMetric_ == ClimateMetric::Wind) {
        return config.climateWindIconId;
      }
      return config.climateRainIconId;
    case LocalApp::Rss:
      if (currentRssUrl_.isEmpty()) {
        return config.rssFallbackIconId;
      } else {
        const uint32_t selected =
            iconForItem(config.rssFeedItems, currentRssUrl_);
        return selected != 0 ? selected
                             : iconStore_.faviconId(currentRssUrl_);
      }
    case LocalApp::Market:
      return iconForItem(config.marketItems, currentMarketSymbol_);
    case LocalApp::Energy:
      if (currentEnergyBand_ == EnergyBand::Low) {
        return config.energyIconLowId;
      }
      if (currentEnergyBand_ == EnergyBand::High) {
        return config.energyIconHighId;
      }
      return config.energyIconAverageId;
    case LocalApp::Battery:
      if (peripherals_.readings().batteryPercent > 50) {
        return config.batteryIconHighId;
      }
      return peripherals_.readings().batteryPercent > 20
                 ? config.batteryIconMediumId
                 : config.batteryIconLowId;
    case LocalApp::Calendar:
      return currentCalendarIconId_ != 0 ? currentCalendarIconId_
                                         : config.calendarIconId;
    case LocalApp::Travel:
      return currentTravelIconId_ != 0 ? currentTravelIconId_ : 11999;
    case LocalApp::Activity:
      return config.activityIconId;
    case LocalApp::Reddit:
      return currentRedditIconId_ != 0 ? currentRedditIconId_
                                       : config.redditIconId;
    case LocalApp::Counter:
      return config.counterIconId;
    case LocalApp::Sync:
      return 0;
  }
  return 0;
}

uint8_t AppEngine::iconLevelFor(LocalApp app) const {
  if (app == LocalApp::Battery) {
    return peripherals_.readings().batteryPercent;
  }
  if (app == LocalApp::Climate) {
    switch (currentClimateMetric_) {
      case ClimateMetric::Temperature:
        return temperatureIconLevel(currentTemperatureC_);
      case ClimateMetric::Humidity:
        return isnan(currentWeatherHumidity_)
                   ? 0
                   : static_cast<uint8_t>(constrain(
                         currentWeatherHumidity_, 0.0f, 100.0f));
      case ClimateMetric::Wind:
        return isnan(currentWindSpeedKph_)
                   ? 0
                   : static_cast<uint8_t>(
                         constrain(currentWindSpeedKph_ * 1.25f,
                                   0.0f, 100.0f));
      case ClimateMetric::Rain:
        return isnan(currentRainRateMmHour_)
                   ? 0
                   : static_cast<uint8_t>(constrain(
                         currentRainRateMmHour_ * 10.0f, 0.0f, 100.0f));
    }
    return 0;
  }
  if (app == LocalApp::Energy) {
    return currentEnergyBand_ == EnergyBand::Low
               ? 20
               : (currentEnergyBand_ == EnergyBand::High ? 90 : 55);
  }
  if (app == LocalApp::Calendar) {
    const time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    return static_cast<uint8_t>(local.tm_mday);
  }
  if (app == LocalApp::Travel) return 50;
  if (app == LocalApp::Activity) return 50;
  if (app == LocalApp::Reddit) {
    return 50;
  }
  if (app == LocalApp::Counter) {
    if (!counterCountDown_ || counterConfiguredDurationSeconds_ == 0) {
      return 50;
    }
    const uint64_t durationMs =
        static_cast<uint64_t>(counterConfiguredDurationSeconds_) * 1000ULL;
    const uint64_t elapsed = min<uint64_t>(counterElapsedMs_, durationMs);
    return static_cast<uint8_t>((durationMs - elapsed) * 100ULL /
                                durationMs);
  }
  return 50;
}

String AppEngine::buildMessage(LocalApp app, const AppConfig& config) {
  const SensorReadings sensors = peripherals_.readings();
  switch (app) {
    case LocalApp::Time: {
      const time_t now = time(nullptr);
      const ClockFace face =
          clockFace(config, now, millis() - appStartedMs_,
                    config.timeAnimation == "ticker"
                        ? clockTickerFaceIndex_
                        : clockTickerFaceIndex_ +
                              (millis() - appStartedMs_) /
                                  max<uint32_t>(
                                      1000UL,
                                      static_cast<uint32_t>(
                                          config.clockTimezoneSeconds) *
                                          1000UL));
      currentClockColor_ = face.color;
      currentClockIconId_ = face.iconId;
      activeClockFaceKey_ = face.key;
      const tm displayedTime = clockTime(face, now);
      currentClockHour_ = static_cast<uint8_t>(displayedTime.tm_hour);
      currentClockMinute_ = static_cast<uint8_t>(displayedTime.tm_min);
      const String value =
          formattedClockTime(displayedTime, config.clock24Hour);
      return value;
    }
    case LocalApp::Climate: {
      currentClimateNetatmo_ = config.climateProvider == "netatmo";
      if (currentClimateNetatmo_) {
        const NetatmoReading netatmo = dataServices_.netatmoSnapshot();
        if (!netatmo.available) {
          currentTemperatureC_ = NAN;
          currentWeatherHumidity_ = NAN;
          currentWeatherPressure_ = NAN;
          currentWindSpeedKph_ = NAN;
          currentWindGustKph_ = NAN;
          currentWindDirectionDegrees_ = -1;
          currentRainRateMmHour_ = NAN;
          currentRainLastHourMm_ = NAN;
          currentRainLast24HoursMm_ = NAN;
          const String reason = netatmo.error;
          return String("Netatmo: ") +
                 (reason.isEmpty() ? tr(TextKey::NoData, config.language)
                                   : reason);
        }
        currentTemperatureC_ = netatmo.temperatureC;
        currentWeatherHumidity_ = netatmo.humidityPercent;
        currentWeatherPressure_ = netatmo.pressureHpa;
        currentWindSpeedKph_ =
            netatmo.windAvailable ? netatmo.windSpeedKph : NAN;
        currentWindGustKph_ =
            netatmo.windAvailable ? netatmo.windGustKph : NAN;
        currentWindDirectionDegrees_ =
            netatmo.windAvailable ? netatmo.windDirectionDegrees : -1;
        currentRainRateMmHour_ =
            netatmo.rainAvailable ? netatmo.rainRateMmHour : NAN;
        currentRainLastHourMm_ =
            netatmo.rainAvailable ? netatmo.rainLastHourMm : NAN;
        currentRainLast24HoursMm_ =
            netatmo.rainAvailable ? netatmo.rainLast24HoursMm : NAN;
      } else if (!sensors.climateAvailable) {
        currentTemperatureC_ = NAN;
        currentWeatherHumidity_ = NAN;
        currentWeatherPressure_ = NAN;
        currentWindSpeedKph_ = NAN;
        currentWindGustKph_ = NAN;
        currentWindDirectionDegrees_ = -1;
        currentRainRateMmHour_ = NAN;
        currentRainLastHourMm_ = NAN;
        currentRainLast24HoursMm_ = NAN;
        return String(tr(TextKey::Inside, config.language)) + " " +
               tr(TextKey::NoData, config.language);
      } else {
        currentTemperatureC_ = sensors.temperatureC;
        currentWeatherHumidity_ = sensors.humidityPercent;
        currentWeatherPressure_ = NAN;
        currentWindSpeedKph_ = NAN;
        currentWindGustKph_ = NAN;
        currentWindDirectionDegrees_ = -1;
        currentRainRateMmHour_ = NAN;
        currentRainLastHourMm_ = NAN;
        currentRainLast24HoursMm_ = NAN;
      }

      bool metricFound = false;
      for (uint8_t attempt = 0; attempt < 4; ++attempt) {
        const ClimateMetric candidate =
            static_cast<ClimateMetric>(climateMetricCursor_ % 4);
        climateMetricCursor_ = (climateMetricCursor_ + 1) % 4;
        const bool available =
            candidate == ClimateMetric::Temperature
                ? !isnan(currentTemperatureC_)
            : candidate == ClimateMetric::Humidity
                ? !isnan(currentWeatherHumidity_)
            : candidate == ClimateMetric::Wind
                ? currentClimateNetatmo_ && !isnan(currentWindSpeedKph_)
                : currentClimateNetatmo_ &&
                      (!isnan(currentRainRateMmHour_) ||
                       !isnan(currentRainLastHourMm_) ||
                       !isnan(currentRainLast24HoursMm_));
        if (!available) continue;
        currentClimateMetric_ = candidate;
        metricFound = true;
        break;
      }
      if (!metricFound) return tr(TextKey::NoData, config.language);

      switch (currentClimateMetric_) {
        case ClimateMetric::Temperature:
          return smartNumber(currentTemperatureC_, "°C",
                             config.language, 1);
        case ClimateMetric::Humidity:
          return smartNumber(currentWeatherHumidity_, "%",
                             config.language, 0);
        case ClimateMetric::Wind:
          // Keep Netatmo's native unit and make the unit—not the reading—the
          // first thing to compact. "km" is unambiguous in this weather
          // context and keeps even two-digit wind speeds inside the 23-pixel
          // text lane without silently changing the numerical scale to m/s.
          return compactMetricNumber(currentWindSpeedKph_, "km/h", "km",
                                     "K", config.language, 1);
        case ClimateMetric::Rain:
          if (!isnan(currentRainRateMmHour_)) {
            return compactMetricNumber(currentRainRateMmHour_, "mm/h",
                                       "mm", "M", config.language, 1);
          }
          if (!isnan(currentRainLastHourMm_)) {
            return compactMetricNumber(currentRainLastHourMm_, "mm", "M",
                                       "M", config.language, 1);
          }
          return compactMetricNumber(currentRainLast24HoursMm_, "mm", "M",
                                     "M", config.language, 1);
      }
      return tr(TextKey::NoData, config.language);
    }
    case LocalApp::Rss: {
      Headline headline;
      size_t count = 0;
      String error;
      if (!dataServices_.headlineAt(rssIndex_, headline, count, error)) {
        currentRssSource_ = "";
        currentRssUrl_ = "";
        return String(tr(TextKey::News, config.language)) + " " +
               (error.isEmpty() ? tr(TextKey::NoData, config.language)
                                : error);
      }
      rssIndex_ = count == 0 ? 0 : (rssIndex_ + 1) % count;
      currentRssSource_ = headline.source;
      currentRssUrl_ = headline.sourceUrl;
      return headline.title;
    }
    case LocalApp::Market: {
      MarketQuote quote;
      size_t count = 0;
      String error;
      if (!dataServices_.marketAt(marketIndex_, quote, count, error)) {
        currentMarketChange_ = NAN;
        currentMarketSymbol_ = "";
        currentMarketOpen_ = false;
        return String("Markets: ") +
               (error.isEmpty() ? tr(TextKey::NoData, config.language)
                                : error);
      }
      marketIndex_ = count == 0 ? 0 : (marketIndex_ + 1) % count;
      currentMarketChange_ = quote.percentChange;
      currentMarketSymbol_ = quote.symbol;
      currentMarketOpen_ = quote.marketOpen;
      currentSparklineCount_ = quote.sparklineCount;
      currentSparklineMarker_ =
          quote.sparklineCount >= 2 ? quote.sparklineCount - 1 : -1;
      for (uint8_t index = 0; index < quote.sparklineCount; ++index) {
        currentSparkline_[index] = quote.sparkline[index];
      }
      return smartCurrency(quote.price, quote.currency, config.language);
    }
    case LocalApp::Energy: {
      const EnergyPriceReading energy = dataServices_.energySnapshot();
      if (!energy.available) {
        currentEnergyBand_ = EnergyBand::Average;
        return String(config.language == "et" ? "Elering: andmed puuduvad"
                                               : "Elering: no data");
      } else {
        currentEnergyBand_ = energy.band;
        currentEnergyLowThreshold_ = energy.lowThresholdCentsKwh;
        currentEnergyHighThreshold_ = energy.highThresholdCentsKwh;
        currentSparklineCount_ = energy.sparklineCount;
        currentSparklineMarker_ = energy.currentSparklineIndex;
        for (uint8_t index = 0; index < energy.sparklineCount; ++index) {
          currentSparkline_[index] = energy.sparkline[index];
        }
        // The selected icon and text colour already communicate the band,
        // while the following chart shows direction and the current point.
        // Keep the text stage to the one value that matters at a glance.
        return smartNumber(energy.currentCentsKwh, "c", config.language,
                           energy.currentCentsKwh < 10.0f ? 2 : 1);
      }
    }
    case LocalApp::Battery:
      return smartNumber(sensors.batteryPercent, "%", config.language, 0);
    case LocalApp::Calendar: {
      CalendarEvent event;
      size_t count = 0;
      String error;
      if (!dataServices_.calendarAt(calendarIndex_, event, count, error)) {
        currentCalendarIconId_ = 0;
        return error.isEmpty()
                   ? tr(TextKey::NoData, config.language)
                   : localizedCalendarError(error, config.language);
      }
      calendarIndex_ = count == 0 ? 0 : (calendarIndex_ + 1) % count;
      currentCalendarIconId_ = event.iconId;
      const String timeText =
          eventCountdown(event.startsAt, time(nullptr), config.language);
      return timeText + " " + event.title;
    }
    case LocalApp::Travel: {
      TravelStopSnapshot stop;
      size_t count = 0;
      String error;
      bool stale = true;
      currentTravelColor_ = config.travelColor;
      if (!dataServices_.travelAt(travelIndex_, stop, count, error, stale)) {
        currentTravelIconId_ = 0;
        const String reason =
            localizedTravelError(
                error.isEmpty() ? String("Travel has no saved departures")
                                : error,
                config.language);
        return reason;
      }
      travelIndex_ = count == 0 ? 0 : (travelIndex_ + 1) % count;
      currentTravelIconId_ = stop.iconId;
      if (stale) currentTravelColor_ = 0x9aa8c7;
      if (stop.departureCount == 0) {
        const String reason = localizedTravelError(
            stop.error.isEmpty() ? error : stop.error, config.language);
        return stop.label + " " +
               (reason.isEmpty() ? tr(TextKey::NoData, config.language)
                                 : reason);
      }
      // Put the route once, before the stop name. Repeating it before both
      // departures looked like two unrelated values ("5 14M · 5 28M") and
      // spent scarce ticker width. When no line filter is configured, derive
      // a compact route label from the two values that fit on the display.
      constexpr uint8_t kDisplayedTravelDepartures = 2;
      String routeLabel = stop.line;
      if (routeLabel.isEmpty()) {
        for (uint8_t index = 0;
             index < stop.departureCount &&
             index < kDisplayedTravelDepartures;
             ++index) {
          const String& route = stop.departures[index].route;
          if (route.isEmpty() || routeLabel == route) {
            continue;
          }
          if (!routeLabel.isEmpty()) routeLabel += "/";
          routeLabel += route;
        }
      }
      String message;
      if (!routeLabel.isEmpty()) message = routeLabel + " ";
      message += stop.label;
      const time_t now = time(nullptr);
      for (uint8_t index = 0;
           index < stop.departureCount &&
           index < kDisplayedTravelDepartures;
           ++index) {
        const TravelDeparture& departure = stop.departures[index];
        const uint32_t shownAt =
            stale && departure.scheduledAt != 0 ? departure.scheduledAt
                                                : departure.expectedAt;
        message += " · ";
        if (index == 0) {
          message += eventCountdown(shownAt, now, config.language);
        } else {
          tm departureTime{};
          const time_t departureEpoch = static_cast<time_t>(shownAt);
          localtime_r(&departureEpoch, &departureTime);
          message += formattedClockTime(departureTime, config.clock24Hour);
        }
      }
      return message;
    }
    case LocalApp::Activity: {
      const ActivitySnapshot activity = dataServices_.activitySnapshot();
      currentActivityColor_ =
          activity.stale && activity.valid ? 0x9aa8c7 : config.activityColor;
      if (!activity.valid) {
        if (activity.reconnectRequired) {
          return config.language == "et" ? "Garmin: ühenda uuesti"
                                         : "Garmin: reconnect";
        }
        return String("Garmin: ") +
               (activity.error.isEmpty()
                    ? tr(TextKey::NoData, config.language)
                    : activity.error);
      }

      // Each visit resumes at the next user-selected, locally cached metric.
      // No network or JSON work occurs here, so even a pinned page is smooth.
      for (uint8_t attempt = 0; attempt < 5; ++attempt) {
        const uint8_t metric = activityMetricCursor_++ % 5U;
        if (metric == 0 && config.activityShowSteps) {
          return smartNumber(activity.stepsToday, "", config.language, 0) +
                 (config.language == "et" ? " sammu" : " steps");
        }
        if (metric == 1 && config.activityShowDailyDistance) {
          return String(config.language == "et" ? "Täna " : "Today ") +
                 smartNumber(activity.distanceTodayM / 1000.0f, "km",
                             config.language, 1);
        }
        const String latest =
            activityTypeLabel(activity.latestType, config.language) + " ";
        if (metric == 2 && config.activityShowLatestDistance &&
            activity.latestDistanceM != 0) {
          return latest +
                 smartNumber(activity.latestDistanceM / 1000.0f, "km",
                             config.language, 1);
        }
        if (metric == 3 && config.activityShowLatestDuration &&
            activity.latestDurationSec != 0) {
          const uint32_t totalMinutes =
              max<uint32_t>(1U, (activity.latestDurationSec + 30U) / 60U);
          String duration;
          if (totalMinutes >= 60U) {
            duration = String(totalMinutes / 60U) +
                       (config.language == "et" ? "T" : "H");
            if (totalMinutes % 60U != 0) {
              duration += " " + String(totalMinutes % 60U) + "M";
            }
          } else {
            duration = String(totalMinutes) + "M";
          }
          return latest + duration;
        }
        if (metric == 4 && config.activityShowLatestCalories &&
            activity.latestCaloriesKcal != 0) {
          return latest + smartNumber(activity.latestCaloriesKcal, "kcal",
                                      config.language, 0);
        }
      }
      return config.language == "et" ? "Aktiivsus: andmed puuduvad"
                                     : "Activity: no data";
    }
    case LocalApp::Reddit: {
      Headline post;
      size_t count = 0;
      String error;
      if (!dataServices_.redditAt(redditIndex_, post, count, error)) {
        currentRedditIconId_ = 0;
        return String("Reddit: ") +
               (error.isEmpty()
                    ? tr(TextKey::NoData, config.language)
                    : error);
      }
      redditIndex_ = count == 0 ? 0 : (redditIndex_ + 1) % count;
      currentRedditIconId_ = post.iconId;
      return post.source + " " + post.title;
    }
    case LocalApp::Counter: {
      const uint64_t elapsed = counterElapsedAt(millis());
      const uint64_t seconds =
          counterCountDown_
              ? (elapsed >=
                         static_cast<uint64_t>(
                             counterConfiguredDurationSeconds_) * 1000ULL
                     ? 0
                     : (static_cast<uint64_t>(
                            counterConfiguredDurationSeconds_) * 1000ULL -
                        elapsed + 999ULL) /
                           1000ULL)
              : elapsed / 1000ULL;
      return formattedCounterTime(seconds);
    }
    case LocalApp::Sync:
      return "";
  }
  return "";
}

void AppEngine::startCurrent(uint32_t nowMs, const AppConfig& config,
                             bool continueApp) {
  if (!appEnabled(current_, config)) {
    current_ = adjacentEnabled(1, config);
    continueApp = false;
  }
  if (!continueApp) {
    appStartedMs_ = nowMs;
    activeClockFaceKey_ = "";
    activeClockText_ = "";
  }
  currentSparklineCount_ = 0;
  currentSparklineMarker_ = -1;
  currentEnergyLowThreshold_ = NAN;
  currentEnergyHighThreshold_ = NAN;
  if (current_ == LocalApp::Counter) {
    updateCounterState(nowMs, config);
    animator_.stop();
    currentStarted_ = true;
    return;
  }
  String message = buildMessage(current_, config);
  if (message.length() > config.maximumMessageChars) {
    message = message.substring(0, config.maximumMessageChars);
  }
  const uint32_t customIconId = customIconFor(current_, config);
  const String animation = animationFor(current_, config);
  const uint32_t appColor =
      current_ == LocalApp::Rss
          ? iconStore_.dominantColor(customIconId, colorFor(current_))
          : colorFor(current_);
  animator_.start(message, parseAnimationStyle(animation),
                  rgb(appColor), config.scrollStepMs,
                  static_cast<uint32_t>(config.appDurationSeconds) * 1000UL,
                  iconFor(current_), iconLevelFor(current_), customIconId,
                  current_ == LocalApp::Time || current_ == LocalApp::Market,
                  animation == "ticker");
  if (current_ == LocalApp::Time) {
    animator_.setAnalogClockTime(currentClockHour_, currentClockMinute_);
    activeClockText_ = message;
  }
  if (current_ == LocalApp::Market) {
    // The generated badge is also the fallback for a selected LaMetric icon
    // that is unavailable or cannot be decoded.
    animator_.setIconLabel(tickerIconLabel(currentMarketSymbol_));
  }
  if (((current_ == LocalApp::Market && config.marketChartEnabled &&
        currentSparklineCount_ >= 2) ||
       (current_ == LocalApp::Energy && config.energyChartEnabled)) &&
      currentSparklineCount_ >= 2) {
    animator_.setSparkline(currentSparkline_, currentSparklineCount_,
                           currentSparklineMarker_,
                           false,
                           current_ == LocalApp::Energy ||
                               currentMarketOpen_,
                           current_ == LocalApp::Energy,
                           currentEnergyLowThreshold_,
                           currentEnergyHighThreshold_);
  }
  currentStarted_ = true;
}

void AppEngine::renderCounter(uint32_t nowMs, const AppConfig& config) {
  updateCounterState(nowMs, config);
  const uint64_t elapsedMs = counterElapsedAt(nowMs);
  const uint64_t durationMs =
      static_cast<uint64_t>(counterConfiguredDurationSeconds_) * 1000ULL;
  const uint64_t shownSeconds =
      counterCountDown_
          ? (elapsedMs >= durationMs
                 ? 0
                 : (durationMs - elapsedMs + 999ULL) / 1000ULL)
          : elapsedMs / 1000ULL;
  String text = formattedCounterTime(shownSeconds);
  if (counterRunning_ && (nowMs / 500UL) % 2 == 0) {
    text.replace(":", " ");
  }

  display_.clear();
  const CRGB color = rgb(colorFor(LocalApp::Counter));
  if (config.counterIconId == 0 ||
      !iconStore_.draw(config.counterIconId, display_, 0, 0, nowMs)) {
    display_.drawIcon(TinyIcon::Hourglass, 0, 0, color, nowMs,
                      iconLevelFor(LocalApp::Counter));
  }
  constexpr int16_t kTextLeft = 9;
  const int16_t x = max<int16_t>(
      kTextLeft,
      kTextLeft +
          (hardware::kMatrixWidth - kTextLeft - measureClockText(text)) / 2);
  display_.drawClockText(x, 0, text, color);
}

void AppEngine::renderClock(uint32_t nowMs, const AppConfig& config) {
  if (nowMs - lastRenderMs_ < 200) {
    return;
  }
  lastRenderMs_ = nowMs;
  const time_t now = time(nullptr);
  const uint32_t faceDuration =
      max<uint32_t>(1000UL,
                    static_cast<uint32_t>(config.clockTimezoneSeconds) *
                        1000UL);
  const ClockFace face =
      clockFace(config, now, nowMs - appStartedMs_,
                clockTickerFaceIndex_ +
                    (config.timeAnimation == "ticker"
                         ? 0
                         : (nowMs - appStartedMs_) / faceDuration));
  currentClockColor_ = face.color;
  currentClockIconId_ = face.iconId;
  const tm displayedTime = clockTime(face, now);
  currentClockHour_ = static_cast<uint8_t>(displayedTime.tm_hour);
  currentClockMinute_ = static_cast<uint8_t>(displayedTime.tm_min);
  String text = formattedClockTime(displayedTime, config.clock24Hour);
  if ((nowMs / 500) % 2 == 0) {
    text.replace(":", " ");
  }
  display_.clear();
  const uint32_t customIconId = face.iconId;
  const uint32_t clockColor = face.color;
  const int16_t left = customIconId == 0 ? 7 : 9;
  const int16_t x =
      left + (hardware::kMatrixWidth - left - measureClockText(text)) / 2;
  display_.drawClockText(x, 0, text, rgb(clockColor));
  if (customIconId == 0 ||
      !iconStore_.draw(customIconId, display_, 0, 0, nowMs)) {
    display_.drawAnalogClock(0, 0, rgb(clockColor), currentClockHour_,
                             currentClockMinute_);
  }
}

void AppEngine::renderRefreshAnimation(uint32_t nowMs) {
  const uint32_t elapsed = nowMs - refreshAnimationStartedMs_;
  display_.clear();
  // The complete 32-pixel baseline is the sync counter. It sits behind the
  // artwork, just like the slideshow indicator. Provider-specific learned
  // durations let it advance one physical pixel at a time during each HTTPS
  // stage while retaining a small completion reserve for slow endpoints.
  const uint8_t progressWidth = min<uint16_t>(
      hardware::kMatrixWidth,
      (static_cast<uint32_t>(dataServices_.syncProgressPermille()) *
           hardware::kMatrixWidth +
       999) /
          1000);
  for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
    display_.setPixel(x, 7,
                      x < progressWidth ? CRGB(42, 45, 49)
                                        : CRGB(4, 5, 6));
  }
  // Full-height globe inspired by LaMetric 4236: a rounded blue sphere with
  // green land drifting behind the edge as the animation advances.
  constexpr uint8_t globeMask[8] = {
      0x3c, 0x7e, 0xff, 0xff, 0xff, 0xff, 0x7e, 0x3c};
  constexpr uint8_t landMap[8] = {
      0x38, 0x70, 0x30, 0x3c, 0x1e, 0x16, 0x18, 0x10};
  const uint8_t drift = (elapsed / 320) & 7;
  for (uint8_t y = 0; y < 8; ++y) {
    for (uint8_t x = 0; x < 8; ++x) {
      if ((globeMask[y] & (0x80 >> x)) == 0) continue;
      const uint8_t sourceX = (x + drift) & 7;
      const bool land = (landMap[y] & (0x80 >> sourceX)) != 0;
      if (land) {
        display_.setPixel(x, y,
                          ((x + y + drift) & 2) ? CRGB(35, 190, 95)
                                                 : CRGB(92, 218, 155));
      } else {
        display_.setPixel(x, y,
                          x < 3 ? CRGB(25, 92, 205)
                                : CRGB(24, 135, 220));
      }
    }
  }

  // A linear red packet scans both directions between globe and satellite,
  // echoing KITT and old Nokia infrared indicators without a permanent guide.
  constexpr uint8_t signalCount = 16;
  const uint8_t signalPhase =
      (elapsed / 75) % static_cast<uint8_t>(signalCount * 2 - 2);
  const bool forward = signalPhase < signalCount;
  const uint8_t signal = forward
                             ? signalPhase
                             : static_cast<uint8_t>(signalCount * 2 - 2 -
                                                    signalPhase);
  const int8_t tailDirection = forward ? -1 : 1;
  const int8_t tail2 = static_cast<int8_t>(signal) + tailDirection * 2;
  const int8_t tail1 = static_cast<int8_t>(signal) + tailDirection;
  if (tail2 >= 0 && tail2 < signalCount) {
    display_.setPixel(8 + tail2, 3, CRGB(55, 0, 0));
  }
  if (tail1 >= 0 && tail1 < signalCount) {
    display_.setPixel(8 + tail1, 3, CRGB(150, 4, 0));
  }
  display_.setPixel(8 + signal, 3, CRGB(255, 28, 0));

  // Satellite inspired by LaMetric 49090. Its cyan solar panels alternate
  // slightly while the central grey body and small status light stay clear.
  const bool satellitePulse = ((elapsed / 500) & 1) != 0;
  const CRGB panelA = satellitePulse ? CRGB(45, 215, 220)
                                     : CRGB(55, 170, 230);
  const CRGB panelB = satellitePulse ? CRGB(55, 170, 230)
                                     : CRGB(45, 215, 220);
  for (uint8_t y = 2; y <= 5; ++y) {
    display_.setPixel(24, y, (y & 1) ? panelA : panelB);
    display_.setPixel(25, y, (y & 1) ? panelB : panelA);
    display_.setPixel(30, y, (y & 1) ? panelB : panelA);
    display_.setPixel(31, y, (y & 1) ? panelA : panelB);
  }
  display_.setPixel(27, 0, CRGB(92, 98, 102));
  display_.setPixel(28, 0, CRGB(92, 98, 102));
  display_.setPixel(27, 1, CRGB(155, 158, 162));
  display_.setPixel(28, 1, satellitePulse ? CRGB(125, 255, 45)
                                          : CRGB(155, 158, 162));
  display_.setPixel(26, 3, CRGB(195, 198, 200));
  display_.setPixel(27, 2, CRGB(78, 83, 86));
  display_.setPixel(28, 2, CRGB(78, 83, 86));
  display_.setPixel(27, 3, CRGB(92, 98, 102));
  display_.setPixel(28, 3, CRGB(92, 98, 102));
  display_.setPixel(27, 4, CRGB(78, 83, 86));
  display_.setPixel(28, 4, CRGB(78, 83, 86));
  display_.setPixel(27, 6, CRGB(175, 178, 180));
  display_.setPixel(28, 6, CRGB(175, 178, 180));
  display_.setPixel(26, 7, CRGB(145, 148, 150));
  display_.setPixel(27, 7, CRGB(175, 178, 180));
  display_.setPixel(28, 7, CRGB(145, 148, 150));
  display_.setPixel(29, 7, CRGB(115, 120, 124));
  display_.show();
}

void AppEngine::renderModeIndicator(uint32_t nowMs,
                                    const AppConfig& config) {
  constexpr uint8_t kLeft = 0;
  constexpr uint8_t kRight = hardware::kMatrixWidth - 1;
  auto backgroundPixel = [this](uint8_t x, const CRGB& color) {
    // Content owns the foreground. Indicators fill only otherwise-black
    // pixels, so an 8x8 icon or moving ticker naturally passes in front.
    if (display_.getPixel(x, hardware::kMatrixHeight - 1) == CRGB::Black) {
      display_.setPixel(x, hardware::kMatrixHeight - 1, color);
    }
  };
  if (paused_) {
    uint8_t enabledCount = 0;
    uint8_t currentPosition = 0;
    for (uint8_t raw = 0;
         raw < static_cast<uint8_t>(LocalApp::Sync); ++raw) {
      const LocalApp candidate = static_cast<LocalApp>(raw);
      if (!appEnabled(candidate, config)) {
        continue;
      }
      if (candidate == current_) {
        currentPosition = enabledCount;
      }
      ++enabledCount;
    }
    if (enabledCount == 0) {
      return;
    }
    const uint8_t width = kRight - kLeft + 1;
    for (uint8_t item = 0; item < enabledCount; ++item) {
      const CRGB color = item == currentPosition ? CRGB(56, 59, 64)
                                                 : CRGB(7, 8, 10);
      // Divide the complete 32-pixel row with integer boundaries. This gives
      // every enabled app either floor(32/N) or ceil(32/N) pixels and leaves
      // no decorative gaps at the edges or between neighbours.
      const uint8_t segmentLeft =
          kLeft + static_cast<uint16_t>(item) * width / enabledCount;
      const uint8_t segmentRight =
          kLeft + static_cast<uint16_t>(item + 1) * width / enabledCount;
      for (uint8_t x = segmentLeft; x < segmentRight; ++x) {
        backgroundPixel(x, color);
      }
    }
    return;
  }

  const uint32_t duration =
      max<uint32_t>(
          static_cast<uint32_t>(config.appDurationSeconds) * 1000UL, 250);
  const uint8_t width = kRight - kLeft + 1;
  const uint8_t filled = min<uint32_t>(
      width, (nowMs - appStartedMs_) * width / duration + 1);
  for (uint8_t x = kLeft; x <= kRight; ++x) {
    backgroundPixel(x, x < kLeft + filled ? CRGB(42, 46, 54)
                                           : CRGB(2, 3, 5));
  }
}

void AppEngine::advanceClockResume(uint32_t nowMs,
                                   const AppConfig& config) {
  if (current_ != LocalApp::Time || !currentStarted_) {
    return;
  }
  if (config.timeAnimation == "ticker") {
    ++clockTickerFaceIndex_;
    return;
  }
  const uint32_t faceDuration =
      max<uint32_t>(1000UL,
                    static_cast<uint32_t>(config.clockTimezoneSeconds) *
                        1000UL);
  const uint32_t elapsed = nowMs - appStartedMs_;
  // Resume at the first face that did not receive a complete display slot.
  // Ceiling division prevents a timezone flashed for one transition frame
  // from being counted as viewed, and still advances a manually skipped app.
  clockTickerFaceIndex_ += static_cast<uint16_t>(
      max<uint32_t>(1, (elapsed + faceDuration - 1) / faceDuration));
}

void AppEngine::tick(uint32_t nowMs, const AppConfig& config) {
  ScopedAppEngineLock lock(mutex_);
  // A deadline scheduler avoids the millisecond-rounding drift of `now-last`
  // pacing. It targets 64 Hz (15.625 ms) to guarantee at least 60 measured
  // frames per second after the WS2812 transfer and scheduler jitter, records
  // deadline for development tests, and deliberately skips stale deadlines
  // after a stall instead of replaying a burst of catch-up frames.
  constexpr uint32_t kFramePeriodUs = 1000000UL / targetFps();
  const uint32_t nowUs = micros();
  if (nextFrameUs_ == 0) {
    nextFrameUs_ = nowUs;
    previousFrameUs_ = nowUs;
    frameWindowStartedUs_ = nowUs;
  }
  if (static_cast<int32_t>(nowUs - nextFrameUs_) < 0) {
    return;
  }
  const uint32_t latenessUs = nowUs - nextFrameUs_;
  // Scheduled sleep intentionally stops calling tick(). Resume with a fresh
  // cadence instead of reporting hours of sleep as missed animation frames.
  const bool resumingAfterIdle = latenessUs >= 1000000UL;
  const uint32_t missed =
      resumingAfterIdle ? 0 : latenessUs / kFramePeriodUs;
  missedFrameDeadlines_ += missed;
  nextFrameUs_ = resumingAfterIdle
                     ? nowUs + kFramePeriodUs
                     : nextFrameUs_ + (missed + 1) * kFramePeriodUs;

  const uint32_t gapUs = nowUs - previousFrameUs_;
  previousFrameUs_ = nowUs;
  if (!resumingAfterIdle) {
    maximumFrameGapUs_ = max(maximumFrameGapUs_, gapUs);
  }
  ++renderedFrames_;
  ++frameWindowFrames_;
  const uint32_t windowUs = nowUs - frameWindowStartedUs_;
  if (windowUs >= 2000000UL) {
    renderFpsTenths_ = static_cast<uint16_t>(min<uint32_t>(
        9999, (frameWindowFrames_ * 10000000ULL + windowUs / 2) / windowUs));
    frameWindowStartedUs_ = nowUs;
    frameWindowFrames_ = 0;
  }

  if (current_ == LocalApp::Sync) {
    const bool beforeMinimum =
        static_cast<int32_t>(refreshAnimationUntilMs_ - nowMs) > 0;
    const bool working = dataServices_.refreshInProgress() ||
                         iconStore_.radioBusy();
    if (beforeMinimum || working) {
      syncQuietSinceMs_ = 0;
      renderRefreshAnimation(nowMs);
      return;
    }
    if (syncQuietSinceMs_ == 0) syncQuietSinceMs_ = nowMs;
    // Give the main task time to discover the next missing icon after the
    // provider queue or previous icon worker becomes idle.
    if (nowMs - syncQuietSinceMs_ < 250) {
      renderRefreshAnimation(nowMs);
      return;
    }
    finishSync(nowMs, config);
  }

  if (notificationUntilMs_ != 0) {
    const bool beforeTimeout =
        static_cast<int32_t>(notificationUntilMs_ - nowMs) > 0;
    if (beforeTimeout && !animator_.finished(nowMs)) {
      animator_.render(display_, nowMs);
      display_.show();
      return;
    }
    notificationUntilMs_ = 0;
    animator_.stop();
    currentStarted_ = false;
  }

  // Auto-low Battery can leave the queue as soon as charge rises above the
  // threshold. Do not leave a manually held Battery page pinned forever once
  // it is no longer eligible (the same guard also handles settings changes).
  if (current_ != LocalApp::Sync && !appEnabled(current_, config)) {
    animator_.stop();
    current_ = adjacentEnabled(1, config);
    currentStarted_ = false;
  }

  if (!currentStarted_) {
    startCurrent(nowMs, config);
  }

  if (current_ == LocalApp::Counter) {
    renderCounter(nowMs, config);
    renderModeIndicator(nowMs, config);
    display_.show();
    const bool targetReached =
        nowMs - appStartedMs_ >=
        static_cast<uint32_t>(config.appDurationSeconds) * 1000UL;
    if (!paused_ && targetReached) {
      advanceAutomatic(nowMs, config);
    }
    return;
  }

  if (current_ == LocalApp::Time) {
    const uint32_t targetMs =
        static_cast<uint32_t>(config.appDurationSeconds) * 1000UL;
    if (config.timeAnimation == "ticker" && animator_.finished(nowMs) &&
        (paused_ || nowMs - appStartedMs_ < targetMs)) {
      ++clockTickerFaceIndex_;
      startCurrent(nowMs, config, true);
    }
    const time_t now = time(nullptr);
    const uint32_t faceDuration =
        max<uint32_t>(1000UL,
                      static_cast<uint32_t>(config.clockTimezoneSeconds) *
                          1000UL);
    const ClockFace face =
        clockFace(config, now, nowMs - appStartedMs_,
                  config.timeAnimation == "ticker"
                      ? clockTickerFaceIndex_
                      : clockTickerFaceIndex_ +
                            (nowMs - appStartedMs_) / faceDuration);
    const String clockText =
        formattedClockTime(clockTime(face, now), config.clock24Hour);
    if (config.timeAnimation != "ticker" &&
        (face.key != activeClockFaceKey_ || clockText != activeClockText_)) {
      startCurrent(nowMs, config, true);
    }
    if (config.timeAnimation == "static" ||
        (config.timeAnimation != "ticker" && animator_.finished(nowMs))) {
      renderClock(nowMs, config);
    } else {
      animator_.render(display_, nowMs);
    }
  } else {
    animator_.render(display_, nowMs);
  }
  renderModeIndicator(nowMs, config);
  display_.show();

  const bool targetReached =
      nowMs - appStartedMs_ >=
      static_cast<uint32_t>(config.appDurationSeconds) * 1000UL;
  const bool normalFinished =
      current_ == LocalApp::Time
          ? (config.timeAnimation == "ticker"
                 ? (targetReached && animator_.finished(nowMs))
                 : targetReached)
          : animator_.finished(nowMs);
  const bool streaming = current_ == LocalApp::Rss ||
                         current_ == LocalApp::Market ||
                         current_ == LocalApp::Calendar ||
                         current_ == LocalApp::Travel ||
                         current_ == LocalApp::Activity ||
                         current_ == LocalApp::Reddit;
  if (paused_) {
    const bool liveClock = current_ == LocalApp::Time;
    if (normalFinished && !liveClock) {
      startCurrent(nowMs, config, true);
    }
  } else if (streaming && normalFinished && !targetReached) {
    startCurrent(nowMs, config, true);
  } else if (normalFinished && targetReached) {
    advanceClockResume(nowMs, config);
    advanceAutomatic(nowMs, config);
  }
}

void AppEngine::showLatestNews(uint32_t headlineId) {
  ScopedAppEngineLock lock(mutex_);
  const AppConfig config = configStore_.snapshot();
  Headline headline;
  if (!dataServices_.headlineById(headlineId, headline)) {
    return;
  }
  currentRssSource_ = headline.source;
  currentRssUrl_ = headline.sourceUrl;
  String message = headline.title;
  if (message.length() > config.maximumMessageChars) {
    message = message.substring(0, config.maximumMessageChars);
  }
  const uint32_t customIconId = customIconFor(LocalApp::Rss, config);
  notify(message,
         iconStore_.dominantColor(customIconId, colorFor(LocalApp::Rss)),
         min<uint32_t>(
             static_cast<uint32_t>(config.maximumAppSeconds) * 1000UL,
             45000UL),
         config.rssAnimation, iconFor(LocalApp::Rss), 50, customIconId,
         config.rssAnimation == "ticker");
}

void AppEngine::showRainStarted(const NetatmoReading& reading) {
  ScopedAppEngineLock lock(mutex_);
  const AppConfig config = configStore_.snapshot();
  if (!config.climateEnabled || config.climateProvider != "netatmo") return;
  currentClimateNetatmo_ = true;
  currentClimateMetric_ = ClimateMetric::Rain;
  currentRainRateMmHour_ = reading.rainRateMmHour;
  currentRainLastHourMm_ = reading.rainLastHourMm;
  currentRainLast24HoursMm_ = reading.rainLast24HoursMm;
  String message = config.language == "et" ? "SAJAB" : "RAIN";
  if (!isnan(reading.rainRateMmHour)) {
    message += " ";
    message += compactMetricNumber(reading.rainRateMmHour, "mm/h", "mm",
                                   "M", config.language, 1);
  }
  notify(message, 0x3977ff,
         min<uint32_t>(
             static_cast<uint32_t>(config.maximumAppSeconds) * 1000UL,
             30000UL),
         config.climateAnimation, TinyIcon::Rain,
         iconLevelFor(LocalApp::Climate), config.climateRainIconId,
         config.climateAnimation == "ticker");
}

void AppEngine::showEnergyPrice() {
  ScopedAppEngineLock lock(mutex_);
  const AppConfig config = configStore_.snapshot();
  if (!config.energyEnabled) {
    return;
  }
  currentSparklineCount_ = 0;
  currentSparklineMarker_ = -1;
  currentEnergyLowThreshold_ = NAN;
  currentEnergyHighThreshold_ = NAN;
  const String message = buildMessage(LocalApp::Energy, config);
  const uint32_t customIconId = customIconFor(LocalApp::Energy, config);
  notify(message, colorFor(LocalApp::Energy),
         min<uint32_t>(
             static_cast<uint32_t>(config.maximumAppSeconds) * 1000UL,
             30000UL),
         config.energyAnimation, iconFor(LocalApp::Energy),
         iconLevelFor(LocalApp::Energy), customIconId);
  if (config.energyChartEnabled && currentSparklineCount_ >= 2) {
    animator_.setSparkline(
        currentSparkline_, currentSparklineCount_, currentSparklineMarker_,
        false, true, true, currentEnergyLowThreshold_,
        currentEnergyHighThreshold_);
  }
}

void AppEngine::showCalendarEvent(const CalendarEvent& event) {
  ScopedAppEngineLock lock(mutex_);
  const AppConfig config = configStore_.snapshot();
  if (!config.calendarEnabled || event.title.isEmpty()) {
    return;
  }
  const String timeText =
      eventCountdown(event.startsAt, time(nullptr), config.language);
  notify(timeText + " " + event.title, config.calendarColor,
         min<uint32_t>(
             static_cast<uint32_t>(config.maximumAppSeconds) * 1000UL,
             45000UL),
         config.calendarAnimation, TinyIcon::Calendar,
         iconLevelFor(LocalApp::Calendar),
         event.iconId != 0 ? event.iconId : config.calendarIconId,
         config.calendarAnimation == "ticker");
}

void AppEngine::notify(const String& text, uint32_t color,
                       uint16_t durationMs, const String& animation,
                       TinyIcon icon, uint8_t iconLevel,
                       uint32_t customIconId,
                       bool scrollIconWithTicker) {
  ScopedAppEngineLock lock(mutex_);
  const AppConfig config = configStore_.snapshot();
  animator_.start(
      text, parseAnimationStyle(animation), rgb(color), config.scrollStepMs,
      durationMs, icon, iconLevel, customIconId, false,
      scrollIconWithTicker);
  notificationUntilMs_ = millis() + durationMs;
}

String AppEngine::currentAppName() const {
  ScopedAppEngineLock lock(mutex_);
  switch (current_) {
    case LocalApp::Time:
      return "time";
    case LocalApp::Climate:
      return "climate";
    case LocalApp::Rss:
      return "rss";
    case LocalApp::Market:
      return "market";
    case LocalApp::Energy:
      return "energy";
    case LocalApp::Battery:
      return "battery";
    case LocalApp::Calendar:
      return "calendar";
    case LocalApp::Travel:
      return "travel";
    case LocalApp::Activity:
      return "activity";
    case LocalApp::Reddit:
      return "reddit";
    case LocalApp::Counter:
      return "counter";
    case LocalApp::Sync:
      return "sync";
  }
  return "unknown";
}

bool AppEngine::refreshAnimationActive() const {
  ScopedAppEngineLock lock(mutex_);
  return current_ == LocalApp::Sync;
}

bool AppEngine::syncAppActive() const {
  ScopedAppEngineLock lock(mutex_);
  return current_ == LocalApp::Sync;
}

void AppEngine::setSuspended(bool suspended, uint32_t nowMs) {
  ScopedAppEngineLock lock(mutex_);
  if (suspended) {
    if (suspendedAtMs_ == 0) suspendedAtMs_ = nowMs;
    return;
  }
  if (suspendedAtMs_ == 0) return;
  const uint32_t pausedFor = nowMs - suspendedAtMs_;
  if (current_ != LocalApp::Sync) {
    appStartedMs_ += pausedFor;
    uint32_t animatorPausedFor = pausedFor;
    if (animator_.running()) {
      const uint32_t animatorStartedMs = animator_.sceneKey();
      if (static_cast<int32_t>(animatorStartedMs - suspendedAtMs_) > 0) {
        // A provider alert may have replaced the scene while the display was
        // asleep. Preserve only the unseen part since that new scene began.
        animatorPausedFor = nowMs - animatorStartedMs;
      }
      animator_.pauseFor(animatorPausedFor);
    }
    if (notificationUntilMs_ != 0) {
      notificationUntilMs_ += animatorPausedFor;
    }
    if (loopStartedMs_ != 0) loopStartedMs_ += pausedFor;
  }
  suspendedAtMs_ = 0;
  lastStandbyMinute_ = UINT32_MAX;
}

void AppEngine::renderStandbyClock(uint32_t nowMs,
                                   const AppConfig& config) {
  ScopedAppEngineLock lock(mutex_);
  const time_t now = time(nullptr);
  const uint32_t minuteKey =
      now > 1672531200 ? static_cast<uint32_t>(now / 60) : 0;
  if (minuteKey == lastStandbyMinute_) return;
  lastStandbyMinute_ = minuteKey;
  String text = "--:--";
  if (now > 1672531200) {
    tm local{};
    localtime_r(&now, &local);
    text = formattedClockTime(local, config.clock24Hour);
  }
  display_.clear();
  if (!iconStore_.draw(kStandbyClockIconId, display_, 0, 0, nowMs)) {
    display_.drawIcon(TinyIcon::Clock, 1, 0, CRGB(88, 110, 150), nowMs);
  }
  constexpr int16_t kTextStart = 9;
  constexpr int16_t kTextWidth = hardware::kMatrixWidth - kTextStart;
  const int16_t x = kTextStart +
                    max<int16_t>(0, (kTextWidth - measureClockText(text)) / 2);
  display_.drawClockText(x, 0, text, CRGB::White);
  display_.show();
}

uint32_t AppEngine::estimatedLoopDurationMs(const AppConfig& config) const {
  ScopedAppEngineLock lock(mutex_);
  if (estimatedLoopDurationMs_ != 0) return estimatedLoopDurationMs_;
  uint8_t enabled = 0;
  for (uint8_t raw = 0;
       raw < static_cast<uint8_t>(LocalApp::Sync); ++raw) {
    if (appEnabled(static_cast<LocalApp>(raw), config)) ++enabled;
  }
  return max<uint32_t>(1, enabled) *
         static_cast<uint32_t>(config.appDurationSeconds) * 1000UL;
}

uint8_t AppEngine::loopsUntilSync(const AppConfig& config) const {
  ScopedAppEngineLock lock(mutex_);
  if (!hasOnlineProviders(config)) return 0;
  if (current_ == LocalApp::Sync) return 0;
  if (firstSyncPending_) return 1;
  return loopsSinceSync_ >= config.syncEveryLoops
             ? 0
             : config.syncEveryLoops - loopsSinceSync_;
}

bool AppEngine::browserTickerScene(uint32_t nowMs,
                                   BrowserTickerScene& scene) const {
  ScopedAppEngineLock lock(mutex_);
  if (refreshAnimationUntilMs_ != 0) return false;
  if (notificationUntilMs_ != 0 && animator_.running()) {
    return animator_.browserTickerScene(nowMs, scene);
  }
  if (current_ == LocalApp::Counter && currentStarted_) {
    const AppConfig config = configStore_.snapshot();
    const uint64_t elapsedMs = counterElapsedAt(nowMs);
    const uint64_t durationMs =
        static_cast<uint64_t>(counterConfiguredDurationSeconds_) * 1000ULL;
    const uint64_t shownSeconds =
        counterCountDown_
            ? (elapsedMs >= durationMs
                   ? 0
                   : (durationMs - elapsedMs + 999ULL) / 1000ULL)
            : elapsedMs / 1000ULL;
    scene.text = formattedCounterTime(shownSeconds);
    scene.previousText = scene.text;
    scene.color = rgb(colorFor(LocalApp::Counter));
    scene.style = AnimationStyle::Static;
    scene.key = counterSceneKey(counterCountDown_, counterRunning_,
                                shownSeconds);
    scene.elapsedMs = 0;
    scene.stepMs = config.scrollStepMs;
    scene.holdMs = 1000;
    scene.baseDurationMs = 1000;
    scene.durationMs = 1000;
    scene.pageCount = 1;
    scene.textWidth = measureClockText(scene.text);
    scene.customIconId = config.counterIconId;
    scene.icon = TinyIcon::Hourglass;
    scene.iconLevel =
        counterCountDown_ && durationMs != 0
            ? static_cast<uint8_t>(
                  (durationMs - min<uint64_t>(elapsedMs, durationMs)) *
                  100ULL / durationMs)
            : 50;
    scene.clockFont = true;
    scene.linkedIcon = false;
    return true;
  }
  return currentStarted_ && animator_.browserTickerScene(nowMs, scene);
}

uint32_t AppEngine::copyDisplayFrame(CRGB* logicalFrame,
                                     uint8_t& brightness) const {
  if (logicalFrame == nullptr) return 0;
  // tick() owns the same recursive lock from clear through FastLED.show(). A
  // browser/test snapshot therefore observes one completed logical frame,
  // never half of the previous frame and half of the next one.
  ScopedAppEngineLock lock(mutex_);
  for (uint8_t y = 0; y < hardware::kMatrixHeight; ++y) {
    for (uint8_t x = 0; x < hardware::kMatrixWidth; ++x) {
      logicalFrame[y * hardware::kMatrixWidth + x] = display_.getPixel(x, y);
    }
  }
  brightness = display_.brightness();
  return display_.frameSequence();
}

uint32_t AppEngine::browserSceneKey() const {
  ScopedAppEngineLock lock(mutex_);
  if (refreshAnimationUntilMs_ == 0 && notificationUntilMs_ == 0 &&
      current_ == LocalApp::Counter && currentStarted_) {
    const uint64_t elapsedMs = counterElapsedAt(millis());
    const uint64_t durationMs =
        static_cast<uint64_t>(counterConfiguredDurationSeconds_) * 1000ULL;
    const uint64_t shownSeconds =
        counterCountDown_
            ? (elapsedMs >= durationMs
                   ? 0
                   : (durationMs - elapsedMs + 999ULL) / 1000ULL)
            : elapsedMs / 1000ULL;
    return counterSceneKey(counterCountDown_, counterRunning_, shownSeconds);
  }
  return refreshAnimationUntilMs_ == 0 ? animator_.sceneKey() : 0;
}

bool isSleepTime(const AppConfig& config, time_t now) {
  if (!config.sleepEnabled || now < 1672531200) {
    return false;
  }
  const int start = parseMinutes(config.sleepStart);
  const int end = parseMinutes(config.sleepEnd);
  if (start < 0 || end < 0 || start == end) {
    return false;
  }
  tm local{};
  localtime_r(&now, &local);
  const int minute = local.tm_hour * 60 + local.tm_min;
  if (start < end) {
    return minute >= start && minute < end;
  }
  return minute >= start || minute < end;
}

}  // namespace pixelos
