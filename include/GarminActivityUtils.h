#pragma once

#include <math.h>
#include <stdint.h>
#include <string.h>

namespace pixelos {

enum class GarminActivityType : uint8_t {
  Other = 0,
  Running,
  Cycling,
  Walking,
  Swimming
};

// Host-testable helpers used by the Garmin provider. Keeping civil-time
// conversion out of newlib avoids timegm(), which is not available in the
// ESP32 Arduino toolchain and previously stopped the firmware build.
inline bool garminAsciiContainsIgnoreCase(const char* value,
                                          const char* needle) {
  if (value == nullptr || needle == nullptr || *needle == '\0') return false;
  for (const char* start = value; *start != '\0'; ++start) {
    const char* left = start;
    const char* right = needle;
    while (*left != '\0' && *right != '\0') {
      char a = *left;
      char b = *right;
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) break;
      ++left;
      ++right;
    }
    if (*right == '\0') return true;
  }
  return false;
}

inline GarminActivityType garminActivityTypeFromKey(const char* value) {
  if (garminAsciiContainsIgnoreCase(value, "run")) {
    return GarminActivityType::Running;
  }
  if (garminAsciiContainsIgnoreCase(value, "cycl") ||
      garminAsciiContainsIgnoreCase(value, "bik")) {
    return GarminActivityType::Cycling;
  }
  if (garminAsciiContainsIgnoreCase(value, "walk") ||
      garminAsciiContainsIgnoreCase(value, "hik")) {
    return GarminActivityType::Walking;
  }
  if (garminAsciiContainsIgnoreCase(value, "swim")) {
    return GarminActivityType::Swimming;
  }
  return GarminActivityType::Other;
}

inline bool garminLeapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

inline uint8_t garminDaysInMonth(int year, unsigned month) {
  static const uint8_t kDays[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && garminLeapYear(year) ? 29 : kDays[month - 1];
}

inline bool garminDigits(const char* value, size_t offset, size_t count,
                         unsigned& result) {
  result = 0;
  for (size_t index = 0; index < count; ++index) {
    const char digit = value[offset + index];
    if (digit < '0' || digit > '9') return false;
    result = result * 10U + static_cast<unsigned>(digit - '0');
  }
  return true;
}

inline uint32_t garminUtcEpoch(int year, unsigned month, unsigned day,
                               unsigned hour, unsigned minute,
                               unsigned second) {
  if (year < 1970 || year > 2106 || month < 1 || month > 12 || day < 1 ||
      day > garminDaysInMonth(year, month) || hour > 23 || minute > 59 ||
      second > 60) {
    return 0;
  }
  int adjustedYear = year - (month <= 2 ? 1 : 0);
  const int era = (adjustedYear >= 0 ? adjustedYear
                                     : adjustedYear - 399) /
                  400;
  const unsigned yearOfEra =
      static_cast<unsigned>(adjustedYear - era * 400);
  const int shiftedMonth = static_cast<int>(month) +
                           (month > 2 ? -3 : 9);
  const unsigned dayOfYear =
      (153U * static_cast<unsigned>(shiftedMonth) + 2U) / 5U + day - 1U;
  const unsigned dayOfEra = yearOfEra * 365U + yearOfEra / 4U -
                            yearOfEra / 100U + dayOfYear;
  const int64_t days = static_cast<int64_t>(era) * 146097LL +
                       dayOfEra - 719468LL;
  const uint64_t epoch = static_cast<uint64_t>(days) * 86400ULL +
                         hour * 3600ULL + minute * 60ULL + second;
  return epoch > UINT32_MAX ? 0 : static_cast<uint32_t>(epoch);
}

inline uint32_t garminIsoUtcEpoch(const char* value) {
  if (value == nullptr || strlen(value) < 19 || value[4] != '-' ||
      value[7] != '-' || (value[10] != 'T' && value[10] != ' ') ||
      value[13] != ':' || value[16] != ':') {
    return 0;
  }
  unsigned year = 0;
  unsigned month = 0;
  unsigned day = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  if (!garminDigits(value, 0, 4, year) ||
      !garminDigits(value, 5, 2, month) ||
      !garminDigits(value, 8, 2, day) ||
      !garminDigits(value, 11, 2, hour) ||
      !garminDigits(value, 14, 2, minute) ||
      !garminDigits(value, 17, 2, second)) {
    return 0;
  }
  return garminUtcEpoch(static_cast<int>(year), month, day, hour, minute,
                        second);
}

inline uint32_t garminRoundedUnsigned(double value,
                                      uint32_t maximum = UINT32_MAX) {
  if (!isfinite(value) || value <= 0.0) return 0;
  const double rounded = floor(value + 0.5);
  return rounded >= static_cast<double>(maximum)
             ? maximum
             : static_cast<uint32_t>(rounded);
}

}  // namespace pixelos
