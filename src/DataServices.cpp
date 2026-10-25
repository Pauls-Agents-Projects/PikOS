#include "DataServices.h"

#include "GarminTokenParser.h"
#include "SafeTlsClient.h"
#include "TallinnSiriParser.h"
#include "TlsHeapGuard.h"
#include "TlsTrust.h"
#include "Version.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_task_wdt.h>

#include <algorithm>

namespace pixelos {
namespace {

constexpr size_t kMaximumRssBytes = 196608;
// A settings page must remain responsive even when a third-party endpoint is
// unhealthy. One central sync will retry on its next loop-requested cycle, so a
// single provider attempt must not monopolise the TC001 radio for tens of
// seconds. Six seconds still covers a normal ESP32 TLS handshake and response
// on this Wi-Fi while placing a hard ceiling on a stalled read.
constexpr uint32_t kHttpTimeoutMs = 6000;
constexpr uint32_t kTravelResponseWallClockMs = 15000;
constexpr uint8_t kForceRss = 1U << 0;
constexpr uint8_t kForceNetatmo = 1U << 1;
constexpr uint8_t kForceMarket = 1U << 2;
constexpr uint8_t kForceEnergy = 1U << 3;
constexpr uint8_t kForceCalendar = 1U << 4;
constexpr uint8_t kForceReddit = 1U << 5;
constexpr uint8_t kForceTravel = 1U << 6;
constexpr uint8_t kForceActivity = 1U << 7;
constexpr uint32_t kProviderServiceGapMs = 1800;
constexpr uint8_t kProviderFailureQuarantineThreshold = 3;
constexpr uint32_t kDerivedCheckIntervalMs = 1000;
constexpr size_t kMaximumCalendarEvents = 20;
constexpr size_t kMaximumCalendarCandidatesPerFeed = 40;
constexpr size_t kMaximumCalendarTitleBytes = 192;
constexpr size_t kMaximumGarminTokenResponseBytes = 8192;
constexpr size_t kMaximumGarminProfileBytes = 24576;
constexpr size_t kMaximumGarminSummaryBytes = 24576;
constexpr size_t kMaximumGarminActivitiesBytes = 49152;
constexpr size_t kMaximumGarminAccessTokenBytes = 6144;
constexpr size_t kMaximumGarminRefreshTokenBytes = 3072;

// Construct the provider TLS context before the settings page can fragment
// general heap. The online worker is strictly serial and web request parsing
// is suspended during each provider, so one reusable context is sufficient.
// Large mbedTLS allocations continue to use the separately guarded arena.
SafeTlsClient providerSecureClient;

SafeTlsClient& resetProviderSecureClient() {
  providerSecureClient.stop();
  configureTrustedTlsClient(providerSecureClient);
  // WiFiClientSecure otherwise permits a TLS handshake to sit for 120
  // seconds, far beyond every provider's HTTP timeout and the Sync estimate.
  return providerSecureClient;
}

uint8_t bitCount(uint8_t value) {
  uint8_t count = 0;
  while (value != 0) {
    count += value & 1U;
    value >>= 1;
  }
  return count;
}

int8_t providerIndex(OnlineProvider provider) {
  const uint8_t raw = static_cast<uint8_t>(provider);
  return raw >= static_cast<uint8_t>(OnlineProvider::Rss) &&
                 raw <= static_cast<uint8_t>(OnlineProvider::Activity)
             ? static_cast<int8_t>(raw - 1)
             : -1;
}

uint8_t providerBit(OnlineProvider provider) {
  switch (provider) {
    case OnlineProvider::Rss:
      return kForceRss;
    case OnlineProvider::Netatmo:
      return kForceNetatmo;
    case OnlineProvider::Market:
      return kForceMarket;
    case OnlineProvider::Energy:
      return kForceEnergy;
    case OnlineProvider::Calendar:
      return kForceCalendar;
    case OnlineProvider::Travel:
      return kForceTravel;
    case OnlineProvider::Reddit:
      return kForceReddit;
    case OnlineProvider::Activity:
      return kForceActivity;
    case OnlineProvider::None:
      return 0;
  }
  return 0;
}

const char* providerLabel(OnlineProvider provider) {
  switch (provider) {
    case OnlineProvider::Rss: return "rss";
    case OnlineProvider::Netatmo: return "netatmo";
    case OnlineProvider::Market: return "market";
    case OnlineProvider::Energy: return "energy";
    case OnlineProvider::Calendar: return "calendar";
    case OnlineProvider::Travel: return "travel";
    case OnlineProvider::Reddit: return "reddit";
    case OnlineProvider::Activity: return "activity";
    case OnlineProvider::None: return "none";
  }
  return "none";
}

uint32_t defaultProviderDurationMs(OnlineProvider provider) {
  // First-cycle estimates reflect the relative work of the existing parsers.
  // Each value is replaced by an EWMA as soon as that provider completes.
  switch (provider) {
    case OnlineProvider::Netatmo:
      return 6500;
    case OnlineProvider::Energy:
      return 5500;
    case OnlineProvider::Calendar:
      return 7500;
    case OnlineProvider::Travel:
      return 6000;
    case OnlineProvider::Rss:
      return 8000;
    case OnlineProvider::Market:
      return 7500;
    case OnlineProvider::Reddit:
      return 6000;
    case OnlineProvider::Activity:
      // Token refresh plus profile (only when uncached), daily summary and
      // one bounded activity-list read.
      return 9500;
    case OnlineProvider::None:
      return 1000;
  }
  return 1000;
}

constexpr OnlineProvider kProviderRunOrder[] = {
    OnlineProvider::Netatmo, OnlineProvider::Energy,
    OnlineProvider::Calendar, OnlineProvider::Travel, OnlineProvider::Rss,
    OnlineProvider::Market, OnlineProvider::Reddit,
    OnlineProvider::Activity};

struct TravelParserContext {
  const std::vector<TravelStopItem>* configured = nullptr;
  std::vector<TravelStopSnapshot>* snapshots = nullptr;
  uint32_t fetchedAt = 0;
  uint16_t returnedStopMask = 0;
};

void collectTravelStop(const char* stopId, void* opaque) {
  TravelParserContext* context =
      static_cast<TravelParserContext*>(opaque);
  if (context == nullptr || context->configured == nullptr ||
      stopId == nullptr) {
    return;
  }
  for (size_t index = 0;
       index < context->configured->size() &&
       index < kMaximumTravelStopItems;
       ++index) {
    if ((*context->configured)[index].stopId.equalsIgnoreCase(stopId)) {
      context->returnedStopMask |= static_cast<uint16_t>(1U << index);
    }
  }
}

void collectTravelDeparture(const pikos::TallinnSiriDeparture& record,
                            void* opaque) {
  TravelParserContext* context =
      static_cast<TravelParserContext*>(opaque);
  if (context == nullptr || context->configured == nullptr ||
      context->snapshots == nullptr) {
    return;
  }
  for (size_t index = 0;
       index < context->configured->size() &&
       index < context->snapshots->size();
       ++index) {
    const TravelStopItem& configured = (*context->configured)[index];
    if (!configured.stopId.equalsIgnoreCase(record.stopId)) {
      continue;
    }
    if (!configured.line.isEmpty() &&
        !configured.line.equalsIgnoreCase(record.route)) continue;

    TravelStopSnapshot& snapshot = (*context->snapshots)[index];
    TravelDeparture candidate;
    candidate.route = record.route;
    candidate.destination = record.destination;
    candidate.expectedAt =
        context->fetchedAt + record.expectedOffsetSeconds;
    const int64_t scheduled =
        static_cast<int64_t>(candidate.expectedAt) +
        record.scheduledOffsetSeconds;
    candidate.scheduledAt = static_cast<uint32_t>(
        max<int64_t>(0, min<int64_t>(UINT32_MAX, scheduled)));
    candidate.realtime = record.realtime;

    bool duplicate = false;
    uint8_t insertAt = snapshot.departureCount;
    for (uint8_t candidateIndex = 0;
         candidateIndex < snapshot.departureCount;
         ++candidateIndex) {
      const TravelDeparture& present =
          snapshot.departures[candidateIndex];
      if (present.expectedAt == candidate.expectedAt &&
          present.route.equalsIgnoreCase(candidate.route)) {
        duplicate = true;
        break;
      }
      if (insertAt == snapshot.departureCount &&
          candidate.expectedAt < present.expectedAt) {
        insertAt = candidateIndex;
      }
    }
    if (duplicate || insertAt >= kMaximumTravelDepartures) continue;
    if (snapshot.departureCount < kMaximumTravelDepartures) {
      ++snapshot.departureCount;
    }
    for (uint8_t move = snapshot.departureCount - 1;
         move > insertAt; --move) {
      snapshot.departures[move] = snapshot.departures[move - 1];
    }
    snapshot.departures[insertAt] = std::move(candidate);
  }
}

EnergyBand energyBand(float price, float lowThreshold, float highThreshold) {
  if (price <= lowThreshold) return EnergyBand::Low;
  if (price >= highThreshold) return EnergyBand::High;
  return EnergyBand::Average;
}

bool localDayBounds(time_t now, uint32_t& dayStart, uint32_t& dayEnd,
                    uint32_t& followingDayEnd) {
  if (now < 1672531200) return false;
  tm local{};
  localtime_r(&now, &local);
  local.tm_hour = 0;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  const time_t start = mktime(&local);
  tm next = local;
  ++next.tm_mday;
  next.tm_isdst = -1;
  const time_t end = mktime(&next);
  tm following = local;
  following.tm_mday += 2;
  following.tm_isdst = -1;
  const time_t end2 = mktime(&following);
  if (start <= 0 || end <= start || end2 <= end) return false;
  dayStart = static_cast<uint32_t>(start);
  dayEnd = static_cast<uint32_t>(end);
  followingDayEnd = static_cast<uint32_t>(end2);
  return true;
}

uint32_t energyCoverageEnd(const std::vector<EnergyPricePoint>& points) {
  if (points.empty()) return 0;
  uint32_t interval = 3600;
  if (points.size() >= 2) {
    const uint32_t candidate =
        points.back().timestamp - points[points.size() - 2].timestamp;
    if (candidate >= 300 && candidate <= 7200) interval = candidate;
  }
  return points.back().timestamp + interval;
}

bool deriveEnergyReading(const EnergyPriceReading& cached,
                         const std::vector<EnergyPricePoint>& points,
                         const String& chartRange, time_t now,
                         EnergyPriceReading& reading) {
  reading = cached;
  reading.available = false;
  reading.currentCentsKwh = NAN;
  reading.averageCentsKwh = NAN;
  reading.lowThresholdCentsKwh = NAN;
  reading.highThresholdCentsKwh = NAN;
  reading.nextDirection = 0;
  reading.nextChangeAt = 0;
  reading.pointsToday = 0;
  reading.sparklineCount = 0;
  reading.currentSparklineIndex = -1;
  reading.chartStartsAt = 0;
  reading.chartEndsAt = 0;
  if (points.empty() || now < 1672531200) return false;

  const uint32_t epoch = static_cast<uint32_t>(now);
  const uint32_t coverageEnd = energyCoverageEnd(points);
  if (epoch < points.front().timestamp || epoch >= coverageEnd) {
    reading.error = "Cached energy prices expired";
    return false;
  }

  size_t currentIndex = 0;
  for (size_t index = 1; index < points.size(); ++index) {
    if (points[index].timestamp > epoch) break;
    currentIndex = index;
  }

  uint32_t dayStart = 0;
  uint32_t dayEnd = 0;
  uint32_t followingDayEnd = 0;
  if (!localDayBounds(now, dayStart, dayEnd, followingDayEnd)) return false;

  float dayPrices[100]{};
  size_t dayCount = 0;
  float dayTotal = 0.0f;
  for (const EnergyPricePoint& point : points) {
    if (point.timestamp >= dayStart && point.timestamp < dayEnd &&
        dayCount < sizeof(dayPrices) / sizeof(dayPrices[0])) {
      dayPrices[dayCount++] = point.centsKwh;
      dayTotal += point.centsKwh;
    }
  }
  if (dayCount == 0) {
    reading.error = "Cached energy prices do not cover today";
    return false;
  }
  std::sort(dayPrices, dayPrices + dayCount);
  const float lowThreshold = dayPrices[(dayCount - 1) / 3];
  const float highThreshold = dayPrices[((dayCount - 1) * 2) / 3];
  const float currentPrice = points[currentIndex].centsKwh;
  const EnergyBand currentBand =
      energyBand(currentPrice, lowThreshold, highThreshold);

  reading.available = true;
  reading.currentCentsKwh = currentPrice;
  reading.averageCentsKwh = dayTotal / static_cast<float>(dayCount);
  reading.lowThresholdCentsKwh = lowThreshold;
  reading.highThresholdCentsKwh = highThreshold;
  reading.band = currentBand;
  reading.pointsToday = static_cast<uint16_t>(dayCount);

  for (size_t index = currentIndex + 1; index < points.size(); ++index) {
    const EnergyBand nextBand =
        energyBand(points[index].centsKwh, lowThreshold, highThreshold);
    if (nextBand != currentBand) {
      reading.nextChangeAt = points[index].timestamp;
      reading.nextDirection =
          points[index].centsKwh > currentPrice ? 1 : -1;
      break;
    }
  }

  uint32_t chartStart = dayStart;
  uint32_t chartEnd = dayEnd;
  if (chartRange == "next12h" || chartRange == "next24h") {
    chartStart = points[currentIndex].timestamp;
    const uint32_t hours = chartRange == "next12h" ? 12UL : 24UL;
    chartEnd = min<uint32_t>(coverageEnd, chartStart + hours * 3600UL);
  } else if (chartRange == "today_tomorrow") {
    chartEnd = min<uint32_t>(coverageEnd, followingDayEnd);
  }

  size_t chartFirst = points.size();
  size_t chartCount = 0;
  for (size_t index = 0; index < points.size(); ++index) {
    if (points[index].timestamp >= chartStart &&
        points[index].timestamp < chartEnd) {
      if (chartFirst == points.size()) chartFirst = index;
      ++chartCount;
    }
  }
  // Late publication of tomorrow's prices must not erase today's useful
  // graph. Falling back here is local work over the existing cache.
  if (chartCount < 2) {
    chartStart = dayStart;
    chartEnd = min<uint32_t>(coverageEnd, dayEnd);
    chartFirst = points.size();
    chartCount = 0;
    for (size_t index = 0; index < points.size(); ++index) {
      if (points[index].timestamp >= chartStart &&
          points[index].timestamp < chartEnd) {
        if (chartFirst == points.size()) chartFirst = index;
        ++chartCount;
      }
    }
  }

  reading.chartStartsAt = chartStart;
  reading.chartEndsAt = chartEnd;
  if (chartFirst != points.size() && chartCount >= 2) {
    reading.sparklineCount =
        static_cast<uint8_t>(min<size_t>(23, chartCount));
    for (uint8_t bin = 0; bin < reading.sparklineCount; ++bin) {
      const size_t first =
          chartFirst + bin * chartCount / reading.sparklineCount;
      const size_t end = chartFirst + max<size_t>(
          (bin + 1) * chartCount / reading.sparklineCount,
          bin * chartCount / reading.sparklineCount + 1);
      float total = 0.0f;
      for (size_t index = first;
           index < min<size_t>(end, chartFirst + chartCount); ++index) {
        total += points[index].centsKwh;
      }
      reading.sparkline[bin] =
          total / static_cast<float>(
                      min<size_t>(end, chartFirst + chartCount) - first);
    }
    if (epoch >= chartStart && epoch < chartEnd) {
      const uint32_t length = max<uint32_t>(1, chartEnd - chartStart);
      const uint32_t elapsed = epoch - chartStart;
      reading.currentSparklineIndex = static_cast<int8_t>(min<uint32_t>(
          reading.sparklineCount - 1,
          elapsed * reading.sparklineCount / length));
    }
  }
  return true;
}

bool liveCalendarEvent(const CalendarEvent& event, uint32_t now) {
  const uint32_t endsAt =
      event.endsAt != 0
          ? event.endsAt
          : event.startsAt + (event.allDay ? 86400UL : 3600UL);
  return endsAt > now;
}

String normalizedMarketSymbol(String symbol) {
  symbol.trim();
  if (symbol.startsWith("$")) {
    symbol.remove(0, 1);
  } else if (symbol.startsWith("€")) {
    symbol.remove(0, String("€").length());
  } else if (symbol.startsWith("£")) {
    symbol.remove(0, String("£").length());
  }
  symbol.trim();
  return symbol;
}

std::vector<String> configuredMarketSymbols(const AppConfig& config) {
  std::vector<String> symbols;
  symbols.reserve(6);
  for (const IconItem& item : parseIconItems(config.marketItems, 6)) {
    const String symbol = normalizedMarketSymbol(item.value);
    if (!symbol.isEmpty()) symbols.push_back(symbol);
  }
  return symbols;
}

bool configuredMarketSymbol(const std::vector<String>& configured,
                            const String& symbol) {
  const String wanted = normalizedMarketSymbol(symbol);
  for (const String& item : configured) {
    if (item.equalsIgnoreCase(wanted)) {
      return true;
    }
  }
  return false;
}

void truncateUtf8(String& value, size_t maximumBytes) {
  if (value.length() <= maximumBytes) return;
  size_t cut = maximumBytes;
  while (cut > 0 &&
         (static_cast<uint8_t>(value[cut]) & 0xc0U) == 0x80U) {
    --cut;
  }
  value.remove(cut);
}

uint32_t fnv1a(const String& value) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < value.length(); ++i) {
    hash ^= static_cast<uint8_t>(value[i]);
    hash *= 16777619u;
  }
  return hash;
}

String sourceFromUrl(const String& url) {
  String lower = url;
  lower.toLowerCase();
  if (lower.indexOf("err.ee") >= 0) {
    return "ERR";
  }
  if (lower.indexOf("postimees") >= 0) {
    return "POSTIMEES";
  }
  const int scheme = url.indexOf("://");
  const int start = scheme >= 0 ? scheme + 3 : 0;
  int end = url.indexOf('/', start);
  if (end < 0) {
    end = url.length();
  }
  String source = url.substring(start, end);
  source.toUpperCase();
  return source;
}

String resolveRedirect(const String& currentUrl, const String& location) {
  if (location.startsWith("http://") || location.startsWith("https://")) {
    return location;
  }
  const int schemeEnd = currentUrl.indexOf("://");
  if (schemeEnd < 0) {
    return "";
  }
  const int pathStart = currentUrl.indexOf('/', schemeEnd + 3);
  const String origin =
      pathStart < 0 ? currentUrl : currentUrl.substring(0, pathStart);
  if (location.startsWith("/")) {
    return origin + location;
  }
  const int lastSlash = currentUrl.lastIndexOf('/');
  return (lastSlash > schemeEnd + 2
              ? currentUrl.substring(0, lastSlash + 1)
              : origin + "/") +
         location;
}

bool isRedirectStatus(int status) {
  return status == HTTP_CODE_MOVED_PERMANENTLY ||
         status == HTTP_CODE_FOUND || status == HTTP_CODE_SEE_OTHER ||
         status == HTTP_CODE_TEMPORARY_REDIRECT ||
         status == HTTP_CODE_PERMANENT_REDIRECT;
}

String decodeXml(String input) {
  input.replace("<![CDATA[", "");
  input.replace("]]>", "");

  String plain;
  plain.reserve(input.length());
  bool insideTag = false;
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    if (c == '<') {
      insideTag = true;
    } else if (c == '>') {
      insideTag = false;
    } else if (!insideTag) {
      plain += c;
    }
  }

  auto appendUtf8 = [](String& target, uint32_t point) {
    if (point < 0x80) {
      target += static_cast<char>(point);
    } else if (point < 0x800) {
      target += static_cast<char>(0xc0 | (point >> 6));
      target += static_cast<char>(0x80 | (point & 0x3f));
    } else if (point < 0x10000) {
      target += static_cast<char>(0xe0 | (point >> 12));
      target += static_cast<char>(0x80 | ((point >> 6) & 0x3f));
      target += static_cast<char>(0x80 | (point & 0x3f));
    }
  };

  String output;
  output.reserve(plain.length());
  for (size_t index = 0; index < plain.length();) {
    if (plain[index] != '&') {
      output += plain[index++];
      continue;
    }
    const int semicolon = plain.indexOf(';', index + 1);
    if (semicolon < 0 || semicolon - static_cast<int>(index) > 14) {
      output += plain[index++];
      continue;
    }
    String entity = plain.substring(index + 1, semicolon);
    String named = entity;
    named.toLowerCase();
    if (named == "amp") {
      output += '&';
    } else if (named == "quot" || named == "ldquo" ||
               named == "rdquo") {
      output += '\"';
    } else if (named == "apos" || named == "lsquo" ||
               named == "rsquo") {
      output += '\'';
    } else if (named == "nbsp") {
      output += ' ';
    } else if (named == "lt") {
      output += '<';
    } else if (named == "gt") {
      output += '>';
    } else if (named == "ndash" || named == "mdash") {
      output += '-';
    } else if (named == "hellip") {
      output += "...";
    } else if (entity.startsWith("#")) {
      const bool hexadecimal =
          entity.length() > 2 && (entity[1] == 'x' || entity[1] == 'X');
      const char* digits = entity.c_str() + (hexadecimal ? 2 : 1);
      char* end = nullptr;
      const uint32_t point = strtoul(digits, &end, hexadecimal ? 16 : 10);
      if (end == digits || *end != '\0') {
        output += '&';
        output += entity;
        output += ';';
      } else if (point == 160) {
        output += ' ';
      } else if (point == 8216 || point == 8217 || point == 8242) {
        output += '\'';
      } else if (point == 8220 || point == 8221) {
        output += '\"';
      } else if (point == 8211 || point == 8212) {
        output += '-';
      } else if (point == 8230) {
        output += "...";
      } else {
        appendUtf8(output, point);
      }
    } else {
      output += '&';
      output += entity;
      output += ';';
    }
    index = semicolon + 1;
  }
  while (output.indexOf("  ") >= 0) {
    output.replace("  ", " ");
  }
  output.trim();
  return output;
}

String xmlAttribute(const String& tag, const String& lowerTag,
                    const char* attribute) {
  const String key = String(attribute) + "=";
  int start = lowerTag.indexOf(key);
  if (start < 0) return "";
  start += key.length();
  while (start < static_cast<int>(tag.length()) && tag[start] == ' ') ++start;
  if (start >= static_cast<int>(tag.length())) return "";
  const char quote = tag[start];
  if (quote == '\"' || quote == '\'') {
    const int end = tag.indexOf(quote, start + 1);
    // Long Atom <link> tags can be truncated by the parser's tag-size guard.
    // Never return a null-buffer String here: the ESP32 Arduino String move
    // operator dereferences that null buffer when replacing a previously
    // populated value. This was the deterministic Reddit-sync boot crash.
    return end < 0 ? String("") : tag.substring(start + 1, end);
  }
  int end = tag.indexOf(' ', start);
  if (end < 0) end = tag.length();
  return tag.substring(start, end);
}

String safeSubreddit(String value) {
  value.trim();
  if (value.startsWith("r/")) value.remove(0, 2);
  String safe;
  safe.reserve(value.length());
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (isalnum(static_cast<unsigned char>(character)) ||
        character == '_' || character == '-') {
      safe += character;
    }
  }
  return safe;
}

int feedMonth(const String& value) {
  static constexpr const char* kMonths[] = {
      "jan", "feb", "mar", "apr", "may", "jun",
      "jul", "aug", "sep", "oct", "nov", "dec"};
  String lowered = value;
  lowered.toLowerCase();
  for (uint8_t index = 0; index < 12; ++index) {
    if (lowered == kMonths[index]) return index + 1;
  }
  return 0;
}

int32_t feedZoneOffsetSeconds(String value) {
  value.trim();
  value.toUpperCase();
  if (value.isEmpty() || value == "Z" || value == "GMT" ||
      value == "UTC") {
    return 0;
  }
  const char sign = value[0];
  if (sign != '+' && sign != '-') return 0;
  value.remove(0, 1);
  value.replace(":", "");
  if (value.length() < 4) return 0;
  const int hours = value.substring(0, 2).toInt();
  const int minutes = value.substring(2, 4).toInt();
  if (hours > 23 || minutes > 59) return 0;
  const int32_t seconds = hours * 3600L + minutes * 60L;
  return sign == '-' ? -seconds : seconds;
}

uint32_t feedEpochWithZone(int year, unsigned month, unsigned day,
                           unsigned hour, unsigned minute, unsigned second,
                           int32_t zoneOffsetSeconds) {
  const uint32_t utc = garminUtcEpoch(year, month, day, hour, minute, second);
  if (utc == 0) return 0;
  const int64_t adjusted = static_cast<int64_t>(utc) - zoneOffsetSeconds;
  return adjusted > 0 && adjusted <= UINT32_MAX
             ? static_cast<uint32_t>(adjusted)
             : 0;
}

uint32_t parseFeedTimestamp(String value) {
  value = decodeXml(value);
  value.trim();
  if (value.length() >= 19 && value[4] == '-' && value[7] == '-' &&
      (value[10] == 'T' || value[10] == ' ')) {
    const uint32_t civil = garminIsoUtcEpoch(value.c_str());
    if (civil == 0) return 0;
    int zoneAt = value.indexOf('Z', 19);
    if (zoneAt < 0) zoneAt = value.indexOf('+', 19);
    if (zoneAt < 0) zoneAt = value.indexOf('-', 19);
    const int32_t offset =
        zoneAt < 0 ? 0 : feedZoneOffsetSeconds(value.substring(zoneAt));
    const int64_t adjusted = static_cast<int64_t>(civil) - offset;
    return adjusted > 0 && adjusted <= UINT32_MAX
               ? static_cast<uint32_t>(adjusted)
               : 0;
  }

  const int comma = value.indexOf(',');
  if (comma >= 0) value = value.substring(comma + 1);
  value.trim();
  String fields[6];
  uint8_t count = 0;
  int start = 0;
  while (start < static_cast<int>(value.length()) && count < 6) {
    while (start < static_cast<int>(value.length()) && value[start] == ' ') {
      ++start;
    }
    int end = value.indexOf(' ', start);
    if (end < 0) end = value.length();
    if (end > start) fields[count++] = value.substring(start, end);
    start = end + 1;
  }
  if (count >= 5 && !isDigit(fields[0][0])) {
    for (uint8_t index = 0; index + 1 < count; ++index) {
      fields[index] = fields[index + 1];
    }
    --count;
  }
  if (count < 4) return 0;
  const int firstColon = fields[3].indexOf(':');
  const int secondColon = fields[3].indexOf(':', firstColon + 1);
  if (firstColon < 0) return 0;
  const unsigned hour = fields[3].substring(0, firstColon).toInt();
  const unsigned minute =
      fields[3].substring(firstColon + 1,
                          secondColon < 0 ? fields[3].length() : secondColon)
          .toInt();
  const unsigned second =
      secondColon < 0 ? 0 : fields[3].substring(secondColon + 1).toInt();
  return feedEpochWithZone(fields[2].toInt(), feedMonth(fields[1]),
                           fields[0].toInt(), hour, minute, second,
                           count >= 5 ? feedZoneOffsetSeconds(fields[4]) : 0);
}

class FeedTitleParser {
 public:
  FeedTitleParser(const String& source, const String& sourceUrl,
                  uint8_t maximumItems,
                  uint16_t maximumChars, std::vector<Headline>& output,
                  const std::vector<IconItem>* dynamicSources = nullptr)
      : source_(source),
        sourceUrl_(sourceUrl),
        maximumItems_(maximumItems),
        maximumChars_(maximumChars),
        output_(output),
        dynamicSources_(dynamicSources) {}

  void feed(char c) {
    if (full()) {
      return;
    }
    if (inTag_) {
      if (c == '>') {
        processTag();
        inTag_ = false;
        tag_ = "";
      } else if (tag_.length() < 256) {
        tag_ += c;
      }
      return;
    }
    if (c == '<') {
      inTag_ = true;
      tag_ = "";
      return;
    }
    if (capturing_ && title_.length() < maximumChars_ * 4U) {
      title_ += c;
    } else if (capturingTimestamp_ && timestamp_.length() < 96U) {
      timestamp_ += c;
    }
  }

  bool full() const {
    return written_ >= maximumItems_;
  }

  size_t count() const { return written_; }

 private:
  void finishEntry() {
    String cleaned = decodeXml(title_);
    if (cleaned.length() > maximumChars_) {
      cleaned = cleaned.substring(0, maximumChars_);
      cleaned += "…";
    }
    if (cleaned.isEmpty()) return;

    String selectedSource = source_;
    uint32_t selectedIconId = 0;
    if (dynamicSources_ != nullptr) {
      const String candidate = safeSubreddit(entrySource_);
      for (const IconItem& item : *dynamicSources_) {
        if (safeSubreddit(item.value).equalsIgnoreCase(candidate)) {
          selectedSource = "r/" + safeSubreddit(item.value);
          selectedIconId = item.iconId;
          break;
        }
      }
    }
    if (written_ >= output_.size()) output_.emplace_back();
    Headline& headline = output_[written_++];
    headline.source = selectedSource;
    headline.sourceUrl = entryUrl_.isEmpty() ? sourceUrl_ : entryUrl_;
    headline.title = cleaned;
    headline.id = fnv1a(headline.sourceUrl + cleaned);
    headline.iconId = selectedIconId;
    headline.publishedAt = parseFeedTimestamp(timestamp_);
  }

  void processTag() {
    String normalized = tag_;
    normalized.trim();
    String lower = normalized;
    lower.toLowerCase();
    const String attributesLower = lower;

    if (lower.startsWith("![cdata[")) {
      if (capturing_) {
        int end = normalized.lastIndexOf("]]");
        if (end < 0) {
          end = normalized.length();
        }
        title_ += normalized.substring(8, end);
      }
      return;
    }

    const int space = lower.indexOf(' ');
    if (space >= 0) {
      lower = lower.substring(0, space);
    }
    const int colon = lower.lastIndexOf(':');
    if (colon >= 0) {
      lower = lower.substring(colon + 1);
    }

    if (lower == "item" || lower == "entry") {
      inEntry_ = true;
      title_ = "";
      entrySource_ = "";
      entryUrl_ = "";
      timestamp_ = "";
      capturingTimestamp_ = false;
      return;
    }
    if (lower == "/item" || lower == "/entry") {
      finishEntry();
      inEntry_ = false;
      capturing_ = false;
      capturingTimestamp_ = false;
      return;
    }
    if (inEntry_ && dynamicSources_ != nullptr) {
      if (attributesLower.startsWith("category ")) {
        const String label =
            xmlAttribute(normalized, attributesLower, "label");
        const String term =
            label.isEmpty()
                ? xmlAttribute(normalized, attributesLower, "term")
                : String("");
        const String& source = label.isEmpty() ? term : label;
        if (!source.isEmpty()) entrySource_ = source;
      } else if (attributesLower.startsWith("link ")) {
        const String link =
            xmlAttribute(normalized, attributesLower, "href");
        if (!link.isEmpty()) entryUrl_ = link;
        if (entrySource_.isEmpty() && !entryUrl_.isEmpty()) {
          String loweredUrl = entryUrl_;
          loweredUrl.toLowerCase();
          const int marker = loweredUrl.indexOf("/r/");
          if (marker >= 0) {
            int end = loweredUrl.indexOf('/', marker + 3);
            if (end < 0) end = loweredUrl.length();
            entrySource_ = entryUrl_.substring(marker + 3, end);
          }
        }
      }
    }
    if (inEntry_ && lower == "title") {
      title_ = "";
      capturing_ = true;
      return;
    }
    if (capturing_ && lower == "/title") {
      capturing_ = false;
      return;
    }
    if (inEntry_ && (lower == "pubdate" || lower == "published" ||
                     lower == "updated" || lower == "date")) {
      timestamp_ = "";
      capturingTimestamp_ = true;
      return;
    }
    if (capturingTimestamp_ &&
        (lower == "/pubdate" || lower == "/published" ||
         lower == "/updated" || lower == "/date")) {
      capturingTimestamp_ = false;
    }
  }

  String source_;
  String sourceUrl_;
  uint8_t maximumItems_;
  uint16_t maximumChars_;
  std::vector<Headline>& output_;
  const std::vector<IconItem>* dynamicSources_ = nullptr;
  size_t written_ = 0;
  bool inTag_ = false;
  bool inEntry_ = false;
  bool capturing_ = false;
  String tag_;
  String title_;
  String entrySource_;
  String entryUrl_;
  String timestamp_;
  bool capturingTimestamp_ = false;
};

String formEncode(const String& value) {
  String output;
  char encoded[4];
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      output += static_cast<char>(c);
    } else {
      snprintf(encoded, sizeof(encoded), "%%%02X", c);
      output += encoded;
    }
  }
  return output;
}

String basicAuthorization(const String& clientId) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  String input = clientId + ":";
  String output;
  output.reserve(((input.length() + 2) / 3) * 4 + 6);
  output = "Basic ";
  for (size_t index = 0; index < input.length(); index += 3) {
    const uint32_t a = static_cast<uint8_t>(input[index]);
    const bool hasB = index + 1 < input.length();
    const bool hasC = index + 2 < input.length();
    const uint32_t b = hasB ? static_cast<uint8_t>(input[index + 1]) : 0;
    const uint32_t c = hasC ? static_cast<uint8_t>(input[index + 2]) : 0;
    const uint32_t block = (a << 16) | (b << 8) | c;
    output += kAlphabet[(block >> 18) & 0x3f];
    output += kAlphabet[(block >> 12) & 0x3f];
    output += hasB ? kAlphabet[(block >> 6) & 0x3f] : '=';
    output += hasC ? kAlphabet[block & 0x3f] : '=';
  }
  return output;
}

bool readBoundedHttpPayload(HTTPClient& http, size_t maximumBytes,
                            const char* provider, String& payload,
                            String& error) {
  payload = "";
  const int contentLength = http.getSize();
  if (contentLength > static_cast<int>(maximumBytes)) {
    error = String(provider) + " response is too large";
    return false;
  }
  const size_t reserveBytes =
      contentLength > 0 ? static_cast<size_t>(contentLength)
                        : min<size_t>(maximumBytes, 8192);
  if (!payload.reserve(reserveBytes)) {
    error = String(provider) + " response allocation failed";
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[384];
  uint32_t lastDataMs = millis();
  while ((http.connected() || stream->available() > 0) &&
         payload.length() < maximumBytes) {
    const int available = stream->available();
    if (available <= 0) {
      if (millis() - lastDataMs > kHttpTimeoutMs) break;
      delay(1);
      continue;
    }
    const size_t wanted = min<size_t>(
        min<int>(available, sizeof(buffer)), maximumBytes - payload.length());
    const int count = stream->read(buffer, wanted);
    if (count <= 0) {
      if (millis() - lastDataMs > kHttpTimeoutMs) break;
      delay(1);
      continue;
    }
    if (!payload.concat(reinterpret_cast<const char*>(buffer), count)) {
      payload = "";
      error = String(provider) + " response allocation failed";
      return false;
    }
    lastDataMs = millis();
    vTaskDelay(1);
  }

  // A known size provides an exact truncation check. For chunked/unknown
  // bodies, reaching the hard cap is rejected conservatively so an attacker
  // or changed API can never make ArduinoJson parse a prefix as complete.
  if ((contentLength >= 0 &&
       payload.length() != static_cast<size_t>(contentLength)) ||
      (contentLength < 0 && payload.length() == maximumBytes)) {
    payload = "";
    error = String(provider) + " response was truncated";
    return false;
  }
  if (payload.isEmpty()) {
    error = String(provider) + " returned an empty response";
    return false;
  }
  return true;
}

void addGarminNativeHeaders(HTTPClient& http) {
  http.setUserAgent("GCM-Android-5.23");
  http.addHeader(
      "X-Garmin-User-Agent",
      "com.garmin.android.apps.connectmobile/5.23; ; "
      "Google/sdk_gphone64_arm64/google; Android/33; Dalvik/2.1.0");
  http.addHeader("X-Garmin-Paired-App-Version", "10861");
  http.addHeader("X-Garmin-Client-Platform", "Android");
  http.addHeader("X-App-Ver", "10861");
  http.addHeader("X-Lang", "en");
  http.addHeader("X-GCExperience", "GC5");
  http.addHeader("Accept-Language", "en-US,en;q=0.9");
}

uint32_t boundedGarminNumber(JsonVariantConst value) {
  return value.isNull() ? 0 : garminRoundedUnsigned(value.as<double>());
}

uint32_t parseGarminStart(JsonObjectConst activity) {
  const uint64_t milliseconds =
      activity["beginTimestamp"].as<uint64_t>();
  if (milliseconds != 0) {
    return static_cast<uint32_t>(
        min<uint64_t>(UINT32_MAX, milliseconds / 1000ULL));
  }
  const String value = activity["startTimeGMT"].as<String>();
  return garminIsoUtcEpoch(value.c_str());
}

time_t parseCompactDateTime(String value, bool& allDay) {
  value.trim();
  allDay = value.length() == 8;
  if ((!allDay && value.length() < 15) || (allDay && value.length() != 8)) {
    return 0;
  }
  tm parsed{};
  parsed.tm_year = value.substring(0, 4).toInt() - 1900;
  parsed.tm_mon = value.substring(4, 6).toInt() - 1;
  parsed.tm_mday = value.substring(6, 8).toInt();
  if (!allDay) {
    parsed.tm_hour = value.substring(9, 11).toInt();
    parsed.tm_min = value.substring(11, 13).toInt();
    parsed.tm_sec = value.substring(13, 15).toInt();
  }
  parsed.tm_isdst = -1;
  // Google emits UTC values with a trailing Z and local/TZID values without
  // one. ESP32's newlib does not expose timegm, so convert UTC civil time to
  // Unix days directly while allowing mktime to apply PikOS's configured TZ
  // to local calendar values.
  if (!value.endsWith("Z")) {
    return mktime(&parsed);
  }
  int year = parsed.tm_year + 1900;
  const unsigned month = parsed.tm_mon + 1;
  const unsigned day = parsed.tm_mday;
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
  const unsigned dayOfYear =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  const int64_t days = static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
  return static_cast<time_t>(days * 86400 + parsed.tm_hour * 3600 +
                             parsed.tm_min * 60 + parsed.tm_sec);
}

String recurrencePart(const String& rule, const char* name) {
  const String prefix = String(name) + "=";
  int start = 0;
  while (start < static_cast<int>(rule.length())) {
    int end = rule.indexOf(';', start);
    if (end < 0) end = rule.length();
    const String part = rule.substring(start, end);
    if (part.startsWith(prefix)) return part.substring(prefix.length());
    start = end + 1;
  }
  return "";
}

bool advanceRecurringEvent(CalendarEvent& event, const String& rule,
                           time_t now) {
  String frequency = recurrencePart(rule, "FREQ");
  frequency.toUpperCase();
  if (frequency != "DAILY" && frequency != "WEEKLY" &&
      frequency != "MONTHLY" && frequency != "YEARLY") {
    return true;
  }
  const uint16_t interval =
      max<uint16_t>(1, recurrencePart(rule, "INTERVAL").toInt());
  const time_t duration = max<time_t>(1, event.endsAt - event.startsAt);
  bool untilAllDay = false;
  const String untilText = recurrencePart(rule, "UNTIL");
  const time_t until = untilText.isEmpty()
                           ? 0
                           : parseCompactDateTime(untilText, untilAllDay);

  // Calendar arithmetic, rather than adding fixed seconds, keeps local
  // recurrences at the same wall-clock time across DST and also handles the
  // monthly/yearly rules commonly emitted by Google Calendar.
  for (uint16_t iteration = 0;
       event.endsAt < now && iteration < 4096; ++iteration) {
    tm next{};
    const time_t currentStart = event.startsAt;
    localtime_r(&currentStart, &next);
    next.tm_isdst = -1;
    if (frequency == "DAILY") {
      next.tm_mday += interval;
    } else if (frequency == "WEEKLY") {
      next.tm_mday += 7 * interval;
    } else if (frequency == "MONTHLY") {
      next.tm_mon += interval;
    } else {
      next.tm_year += interval;
    }
    const time_t nextStart = mktime(&next);
    if (nextStart <= event.startsAt || (until != 0 && nextStart > until)) {
      return false;
    }
    event.startsAt = nextStart;
    event.endsAt = nextStart + duration;
  }
  return event.endsAt >= now && (until == 0 || event.startsAt <= until);
}

uint32_t parseIcalTriggerSeconds(String value) {
  value.trim();
  if (!value.startsWith("-P")) {
    return 0;
  }
  value.remove(0, 2);
  if (value.startsWith("T")) {
    value.remove(0, 1);
  }
  uint32_t seconds = 0;
  String digits;
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    if (isdigit(static_cast<unsigned char>(character))) {
      digits += character;
      continue;
    }
    const uint32_t amount = digits.toInt();
    digits = "";
    if (character == 'D') {
      seconds += amount * 86400UL;
    } else if (character == 'H') {
      seconds += amount * 3600UL;
    } else if (character == 'M') {
      seconds += amount * 60UL;
    }
  }
  return seconds;
}

String decodeIcalText(String value) {
  value.replace("\\n", " ");
  value.replace("\\N", " ");
  value.replace("\\,", ",");
  value.replace("\\;", ";");
  value.replace("\\\\", "\\");
  value.trim();
  return value;
}

template <typename T>
float jsonFloatOrNan(const T& value) {
  return value.isNull() ? NAN : value.template as<float>();
}

struct MarketWindow {
  const char* interval;
  const char* range;
  uint16_t frankfurterDays;
  uint16_t krakenIntervalMinutes;
  uint16_t krakenDays;
};

MarketWindow marketWindow(const String& configuredRange) {
  if (configuredRange == "1d") {
    // Ask Frankfurter for a few calendar days so weekends still yield two
    // daily reference rates.
    return {"5m", "1d", 4, 60, 1};
  }
  if (configuredRange == "1mo") {
    return {"1d", "1mo", 35, 1440, 35};
  }
  if (configuredRange == "6mo") {
    return {"1d", "6mo", 190, 10080, 190};
  }
  if (configuredRange == "1y") {
    return {"1wk", "1y", 370, 10080, 370};
  }
  return {"1h", "5d", 10, 240, 5};
}

bool fetchKrakenCryptoPair(const String& displaySymbol, MarketQuote& quote,
                           const MarketWindow& window, String& error) {
  const int dash = displaySymbol.indexOf('-');
  if (dash <= 0 || dash >= static_cast<int>(displaySymbol.length()) - 1) {
    return false;
  }
  String base = displaySymbol.substring(0, dash);
  String currency = displaySymbol.substring(dash + 1);
  base.trim();
  currency.trim();
  base.toUpperCase();
  currency.toUpperCase();
  if (base == "XBT") {
    base = "BTC";
  }
  if (base != "BTC" ||
      (currency != "EUR" && currency != "USD" && currency != "GBP")) {
    return false;
  }

  const String resultKey = base + "/" + currency;
  // Kraken accepts modern BTC symbols in the request, but the JSON result
  // key can still use its legacy asset naming (for example XXBTZEUR). Keep
  // both forms in the narrow filter so the complete OHLC payload never has
  // to reside in the ArduinoJson document.
  const String legacyResultKey = "XXBTZ" + currency;
  const String compactResultKey = base + currency;
  const time_t since =
      time(nullptr) - static_cast<time_t>(window.krakenDays) * 86400;
  const String url =
      "https://api.kraken.com/0/public/OHLC?pair=" + base + currency +
      "&assetVersion=1&interval=" +
      String(window.krakenIntervalMinutes) + "&since=" + String(since);
  String payload;
  int status = 0;
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    SafeTlsClient& client = resetProviderSecureClient();
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
      status = HTTPC_ERROR_CONNECTION_REFUSED;
    } else {
      http.setUserAgent(kUserAgent);
      http.addHeader("Accept", "application/json");
      http.addHeader("Accept-Encoding", "identity");
      http.addHeader("Connection", "close");
      status = http.GET();
      if (status >= 200 && status < 300) {
        payload = http.getString();
      }
      http.end();
    }
    if (!payload.isEmpty()) {
      break;
    }
    if (attempt < 2) {
      vTaskDelay(pdMS_TO_TICKS(450 + attempt * 350));
    }
  }
  if (payload.isEmpty()) {
    const String detail = status < 0 ? HTTPClient::errorToString(status)
                                     : String(status);
    error = displaySymbol + ": Kraken HTTP " + detail;
    return false;
  }

  std::vector<float> history;
  const int errorKey = payload.indexOf("\"error\"");
  const int errorStart =
      errorKey < 0 ? -1 : payload.indexOf('[', errorKey);
  const int errorEnd =
      errorStart < 0 ? -1 : payload.indexOf(']', errorStart);
  if (errorStart < 0 || errorEnd < 0) {
    error = displaySymbol + ": Kraken malformed response";
    return false;
  }
  String providerError = payload.substring(errorStart + 1, errorEnd);
  providerError.trim();
  if (!providerError.isEmpty()) {
    error = displaySymbol + ": Kraken " + providerError;
    return false;
  }

  const int resultStart = payload.indexOf("\"result\"", errorEnd);
  int pairKey =
      payload.indexOf("\"" + resultKey + "\"", resultStart);
  if (pairKey < 0) {
    pairKey = payload.indexOf("\"" + legacyResultKey + "\"",
                              resultStart);
  }
  if (pairKey < 0) {
    pairKey = payload.indexOf("\"" + compactResultKey + "\"",
                              resultStart);
  }
  const int rowsStart =
      pairKey < 0 ? -1 : payload.indexOf('[', pairKey);
  if (rowsStart < 0) {
    error = displaySymbol + ": Kraken pair missing";
    return false;
  }

  // OHLC rows are flat arrays. Read only field 4 (close) directly from the
  // response, avoiding the surprisingly expensive nested ArduinoJson array.
  uint8_t depth = 0;
  uint8_t field = 0;
  for (int cursor = rowsStart;
       cursor < static_cast<int>(payload.length()); ++cursor) {
    const char current = payload[cursor];
    if (current == '[') {
      ++depth;
      if (depth == 2) {
        field = 0;
      }
      continue;
    }
    if (current == ']') {
      if (depth == 0) {
        break;
      }
      --depth;
      if (depth == 0) {
        break;
      }
      continue;
    }
    if (depth != 2) {
      continue;
    }
    if (current == ',') {
      ++field;
      continue;
    }
    if (current == '"') {
      const int end = payload.indexOf('"', cursor + 1);
      if (end < 0) {
        break;
      }
      if (field == 4) {
        const float close = payload.substring(cursor + 1, end).toFloat();
        if (!isnan(close) && close > 0.0f) {
          history.push_back(close);
        }
      }
      cursor = end;
    }
  }
  if (history.empty()) {
    error = displaySymbol + ": Kraken returned no prices";
    return false;
  }

  quote.available = true;
  quote.symbol = displaySymbol;
  quote.name = "Bitcoin";
  quote.currency = currency;
  quote.price = history.back();
  quote.percentChange =
      history.size() >= 2 && history[history.size() - 2] != 0.0f
          ? (history.back() - history[history.size() - 2]) /
                history[history.size() - 2] * 100.0f
          : NAN;
  quote.marketOpen = true;
  quote.sparklineCount = min<size_t>(23, history.size());
  for (uint8_t bin = 0; bin < quote.sparklineCount; ++bin) {
    const size_t first = bin * history.size() / quote.sparklineCount;
    const size_t end = max<size_t>(
        first + 1, (bin + 1) * history.size() / quote.sparklineCount);
    float total = 0.0f;
    const size_t boundedEnd = min<size_t>(end, history.size());
    for (size_t index = first; index < boundedEnd; ++index) {
      total += history[index];
    }
    quote.sparkline[bin] =
        total / static_cast<float>(boundedEnd - first);
  }
  error = "";
  return true;
}

bool fetchFrankfurterPair(const String& displaySymbol, MarketQuote& quote,
                          const MarketWindow& window, String& error) {
  const int slash = displaySymbol.indexOf('/');
  if (slash <= 0 || slash >= static_cast<int>(displaySymbol.length()) - 1) {
    return false;
  }
  String base = displaySymbol.substring(0, slash);
  String currency = displaySymbol.substring(slash + 1);
  base.trim();
  currency.trim();
  base.toUpperCase();
  currency.toUpperCase();
  if (base.length() != 3 || currency.length() != 3) {
    return false;
  }

  const time_t now = time(nullptr);
  const time_t from =
      now - static_cast<time_t>(window.frankfurterDays) * 86400;
  tm fromUtc{};
  gmtime_r(&from, &fromUtc);
  char fromDate[12];
  strftime(fromDate, sizeof(fromDate), "%Y-%m-%d", &fromUtc);

  const String url =
      "https://api.frankfurter.dev/v2/rates?base=" + formEncode(base) +
      "&quotes=" + formEncode(currency) + "&from=" + fromDate;
  String payload;
  int status = 0;
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    SafeTlsClient& client = resetProviderSecureClient();
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
      status = HTTPC_ERROR_CONNECTION_REFUSED;
    } else {
      http.setUserAgent(kUserAgent);
      http.addHeader("Accept", "application/json");
      http.addHeader("Accept-Encoding", "identity");
      http.addHeader("Connection", "close");
      status = http.GET();
      if (status >= 200 && status < 300) {
        payload = http.getString();
      }
      http.end();
    }
    if (!payload.isEmpty()) {
      break;
    }
    if (attempt < 2) {
      vTaskDelay(pdMS_TO_TICKS(450 + attempt * 350));
    }
  }
  if (payload.isEmpty()) {
    const String detail = status < 0 ? HTTPClient::errorToString(status)
                                     : String(status);
    error = displaySymbol + ": Frankfurter HTTP " + detail;
    return false;
  }
  // Buffer the small response. Some TLS servers briefly report an empty
  // stream between records, which ArduinoJson interprets as invalid input.
  DynamicJsonDocument document(6144);
  const DeserializationError parseError =
      deserializeJson(document, payload);
  if (parseError || !document.is<JsonArray>()) {
    error = displaySymbol + ": Frankfurter JSON " +
            String(parseError.c_str()) + " (" + payload.length() + " bytes)";
    return false;
  }

  std::vector<float> history;
  for (JsonObject row : document.as<JsonArray>()) {
    const float rate = jsonFloatOrNan(row["rate"]);
    if (!isnan(rate)) {
      history.push_back(rate);
    }
  }
  if (history.empty()) {
    error = displaySymbol + ": Frankfurter returned no rates";
    return false;
  }
  quote.available = true;
  quote.symbol = displaySymbol;
  quote.name = displaySymbol;
  quote.currency = currency;
  quote.price = history.back();
  quote.percentChange =
      history.size() >= 2 && history[history.size() - 2] != 0.0f
          ? (history.back() - history[history.size() - 2]) /
                history[history.size() - 2] * 100.0f
          : NAN;
  // These are daily central-bank reference rates rather than a live tick.
  quote.marketOpen = false;
  quote.sparklineCount = min<size_t>(23, history.size());
  const size_t historyStart = history.size() - quote.sparklineCount;
  for (uint8_t index = 0; index < quote.sparklineCount; ++index) {
    quote.sparkline[index] = history[historyStart + index];
  }
  error = "";
  return true;
}

}  // namespace

const char* garminActivityTypeName(GarminActivityType type) {
  switch (type) {
    case GarminActivityType::Running:
      return "run";
    case GarminActivityType::Cycling:
      return "ride";
    case GarminActivityType::Walking:
      return "walk";
    case GarminActivityType::Swimming:
      return "swim";
    case GarminActivityType::Other:
    default:
      return "activity";
  }
}

DataServices::DataServices(ConfigStore& configStore)
    : configStore_(configStore) {}

void DataServices::begin() {
  mutex_ = xSemaphoreCreateMutex();
  // The TLS handshake genuinely needs a 12 KiB task stack on this ESP32.
  // Reserve it statically so refreshes neither allocate nor fragment the
  // runtime heap; that keeps one contiguous mbedTLS arena available forever.
  workerTask_ = xTaskCreateStaticPinnedToCore(
      taskEntry, "online-data", kWorkerStackBytes, this, 0,
      reinterpret_cast<StackType_t*>(workerStack_), &workerTaskBuffer_, 0);
  if (workerTask_ == nullptr) {
    Serial.println(F("[data] Could not start online-data worker"));
  }
  beginTlsHeapGuard();
}

void DataServices::tickLocal(uint32_t nowMs) {
  if (mutex_ == nullptr ||
      (lastDerivedCheckMs_ != 0 &&
       nowMs - lastDerivedCheckMs_ < kDerivedCheckIntervalMs)) {
    return;
  }
  lastDerivedCheckMs_ = nowMs;
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  if (now <= 1672531200) return;

  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (!calendarAlertEvent_) {
    for (CalendarEvent& event : data_.calendarEvents) {
      if (liveCalendarEvent(event, now) && event.alertAt != 0 &&
          event.alertAt <= now && event.startsAt > now &&
          event.id != previousCalendarAlertId_) {
        pendingCalendarAlert_ = event;
        previousCalendarAlertId_ = event.id;
        calendarAlertEvent_ = true;
        // A single cached event must never alternate/repeat with another due
        // alarm before the next provider sync.
        event.alertAt = 0;
        break;
      }
    }
  }

  EnergyPriceReading liveEnergy;
  if (deriveEnergyReading(data_.energy, data_.energyPoints,
                          data_.energyChartRange, now, liveEnergy)) {
    const int8_t nextBand = static_cast<int8_t>(liveEnergy.band);
    if (previousEnergyBand_ >= 0 && nextBand != previousEnergyBand_) {
      energyBandEvent_ = true;
    }
    previousEnergyBand_ = nextBand;
  }
  xSemaphoreGive(mutex_);
}

void DataServices::tick(bool wifiConnected, uint32_t nowMs,
                        bool allowProviderStart) {
  tickLocal(nowMs);
  if (taskRunning_ || !allowProviderStart) return;
  if (!wifiConnected) {
    // A missing network must not trap the slideshow in its Sync app forever.
    // Allow a brief reconnect window, then finish the cycle as aborted while
    // retaining every previously cached reading.
    if (forceRefreshMask_ != 0 && syncRequestedAtMs_ != 0) {
      if (offlineSinceMs_ == 0) offlineSinceMs_ = nowMs;
    } else {
      offlineSinceMs_ = 0;
    }
    if (forceRefreshMask_ != 0 && offlineSinceMs_ != 0 &&
        nowMs - offlineSinceMs_ >= 3000) {
      const bool hadStarted = syncActive_;
      forceRefreshMask_ = 0;
      syncActive_ = false;
      syncCompleted_ = 0;
      syncTotal_ = 0;
      lastSyncDurationMs_ =
          nowMs - (hadStarted ? syncStartedAtMs_ : syncRequestedAtMs_);
      if (!hadStarted) ++syncCyclesStarted_;
      ++syncCyclesCompleted_;
      ++syncCyclesAborted_;
      offlineSinceMs_ = 0;
      Serial.println(F("[data] Sync skipped: Wi-Fi offline"));
    }
    return;
  }
  offlineSinceMs_ = 0;
  const uint32_t jobDelayRemaining = nextJobEarliestMs_ - nowMs;
  if (nextJobEarliestMs_ != 0 && jobDelayRemaining > 0 &&
      jobDelayRemaining <= kProviderServiceGapMs) {
    return;
  }
  nextJobEarliestMs_ = 0;

  const AppConfig config = configStore_.snapshot();
  uint8_t enabledForceMask = 0;
  if (config.rssEnabled) enabledForceMask |= kForceRss;
  if (config.climateEnabled && config.climateProvider == "netatmo") {
    enabledForceMask |= kForceNetatmo;
  }
  if (config.marketEnabled) enabledForceMask |= kForceMarket;
  if (config.energyEnabled) enabledForceMask |= kForceEnergy;
  if (config.calendarEnabled) enabledForceMask |= kForceCalendar;
  if (config.travelEnabled &&
      !parseTravelStopItems(config.travelStopItems).empty()) {
    enabledForceMask |= kForceTravel;
  }
  if (config.redditEnabled) enabledForceMask |= kForceReddit;
  if (config.activityEnabled && !config.garminClientId.isEmpty() &&
      !config.garminRefreshToken.isEmpty()) {
    enabledForceMask |= kForceActivity;
  }
  enabledForceMask &= static_cast<uint8_t>(~quarantinedProviderMask_);
  forceRefreshMask_ &= enabledForceMask;
  if (syncActive_ && forceRefreshMask_ == 0) {
    // Settings may disable the remaining providers between serialized jobs.
    // End that cycle instead of leaving the Sync app waiting on an empty mask.
    syncActive_ = false;
    lastSyncDurationMs_ = nowMs - syncStartedAtMs_;
    averageSyncDurationMs_ =
        averageSyncDurationMs_ == 0
            ? lastSyncDurationMs_
            : (averageSyncDurationMs_ * 3UL + lastSyncDurationMs_) / 4UL;
    ++syncCyclesCompleted_;
    ++syncCyclesAborted_;
    return;
  }
  if (forceRefreshMask_ != 0 && !syncActive_) {
    syncActive_ = true;
    syncCompleted_ = 0;
    syncTotal_ = bitCount(forceRefreshMask_);
    syncStartedProviderMask_ = forceRefreshMask_;
    for (OnlineProvider provider : kProviderRunOrder) {
      const int8_t index = providerIndex(provider);
      if (index < 0) continue;
      const uint32_t learned = providerAverageDurationMs_[index];
      syncProviderEstimateMs_[index] =
          learned == 0 ? defaultProviderDurationMs(provider) : learned;
    }
    syncStartedAtMs_ = nowMs;
    ++syncCyclesStarted_;
    Serial.printf("[data] Starting central sync: %u provider(s)\n",
                  static_cast<unsigned>(syncTotal_));
  }
  const uint8_t forced = forceRefreshMask_;
  const bool rssDue = (forced & kForceRss) != 0;
  const bool netatmoDue = (forced & kForceNetatmo) != 0;
  const bool marketDue = (forced & kForceMarket) != 0;
  const bool energyDue = (forced & kForceEnergy) != 0;
  const bool calendarDue = (forced & kForceCalendar) != 0;
  const bool travelDue = (forced & kForceTravel) != 0;
  const bool redditDue = (forced & kForceReddit) != 0;
  const bool activityDue = (forced & kForceActivity) != 0;
  if (!rssDue && !netatmoDue && !marketDue && !energyDue && !calendarDue &&
      !travelDue && !redditDue && !activityDue) {
    return;
  }

  // Run one provider family per worker wake. A full refresh is therefore a
  // short queue rather than one giant allocation lifetime: each result is
  // committed and all parser/TLS temporaries are destroyed before the next
  // due provider starts on the following loop pass.
  const bool runNetatmo = netatmoDue;
  const bool runEnergy = !runNetatmo && energyDue;
  const bool runCalendar = !runNetatmo && !runEnergy && calendarDue;
  const bool runTravel = !runNetatmo && !runEnergy && !runCalendar &&
                         travelDue;
  const bool runRss = !runNetatmo && !runEnergy && !runCalendar &&
                      !runTravel && rssDue;
  const bool runMarket = !runNetatmo && !runEnergy && !runCalendar &&
                         !runTravel && !runRss && marketDue;
  const bool runReddit = !runNetatmo && !runEnergy && !runCalendar &&
                         !runTravel && !runRss && !runMarket && redditDue;
  const bool runActivity = !runNetatmo && !runEnergy && !runCalendar &&
                           !runTravel && !runRss && !runMarket &&
                           !runReddit && activityDue;
  if (runRss) {
    Serial.printf("[data] Queue rss (%u/%u complete)\n",
                  static_cast<unsigned>(syncCompleted_),
                  static_cast<unsigned>(syncTotal_));
  }
  uint8_t startedForceBit = 0;
  if (runRss) startedForceBit = kForceRss;
  if (runNetatmo) startedForceBit = kForceNetatmo;
  if (runMarket) startedForceBit = kForceMarket;
  if (runEnergy) startedForceBit = kForceEnergy;
  if (runCalendar) startedForceBit = kForceCalendar;
  if (runTravel) startedForceBit = kForceTravel;
  if (runReddit) startedForceBit = kForceReddit;
  if (runActivity) startedForceBit = kForceActivity;
  if (workerTask_ == nullptr) {
    syncActive_ = false;
    forceRefreshMask_ = 0;
    ++syncCyclesCompleted_;
    ++syncCyclesAborted_;
    return;
  }
  forceRefreshMask_ &= static_cast<uint8_t>(~startedForceBit);
  // One ESP32 radio serves both directions. Provider jobs are separated by a
  // guaranteed main-loop service window; large preview/settings handlers
  // return busy while the worker itself owns TLS.
  pendingRss_ = runRss;
  pendingNetatmo_ = runNetatmo;
  pendingMarket_ = runMarket;
  pendingEnergy_ = runEnergy;
  pendingCalendar_ = runCalendar;
  pendingTravel_ = runTravel;
  pendingReddit_ = runReddit;
  pendingActivity_ = runActivity;
  activeProvider_ = runRss       ? OnlineProvider::Rss
                    : runNetatmo ? OnlineProvider::Netatmo
                    : runMarket  ? OnlineProvider::Market
                    : runEnergy  ? OnlineProvider::Energy
                    : runCalendar ? OnlineProvider::Calendar
                    : runTravel ? OnlineProvider::Travel
                    : runReddit  ? OnlineProvider::Reddit
                    : runActivity ? OnlineProvider::Activity
                                 : OnlineProvider::None;
  providerStartedAtMs_ = nowMs;
  taskRunning_ = true;
  xTaskNotifyGive(workerTask_);
}

void DataServices::taskEntry(void* argument) {
  DataServices* owner = static_cast<DataServices*>(argument);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const bool fetchRss = owner->pendingRss_;
    const bool fetchNetatmo = owner->pendingNetatmo_;
    const bool fetchMarket = owner->pendingMarket_;
    const bool fetchEnergy = owner->pendingEnergy_;
    const bool fetchCalendar = owner->pendingCalendar_;
    const bool fetchTravel = owner->pendingTravel_;
    const bool fetchReddit = owner->pendingReddit_;
    const bool fetchActivity = owner->pendingActivity_;
    const char* provider = fetchNetatmo ? "netatmo"
                           : fetchEnergy ? "energy"
                           : fetchCalendar ? "calendar"
                           : fetchTravel ? "travel"
                           : fetchRss ? "rss"
                           : fetchMarket ? "market"
                           : fetchReddit ? "reddit"
                           : fetchActivity ? "activity"
                                         : "none";
    Serial.printf("[data] Refreshing %s\n", provider);
    // mbedTLS can remain inside one handshake call longer than ESP-IDF's
    // five-second IDLE0 watchdog window on a weak Wi-Fi link. Every provider
    // has its own connect/read timeout, so temporarily unwatch the idle task
    // around this bounded operation and restore it as soon as the call exits.
    TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCPU(0);
    if (idle0 != nullptr) esp_task_wdt_delete(idle0);
    bool providerOk = false;
    {
      const AppConfig config = owner->configStore_.snapshot();
      providerOk = owner->runFetch(config, fetchRss, fetchNetatmo, fetchMarket,
                                   fetchEnergy, fetchCalendar, fetchTravel,
                                   fetchReddit, fetchActivity);
    }
    const bool linkLost = WiFi.status() != WL_CONNECTED;
    if (linkLost) {
      // Do not let the remaining central queue hammer DNS/TCP after a provider
      // loses Wi-Fi. Failed sockets consume ESP32's tiny lwIP timeout pool and
      // can otherwise leave port 80 unable to send even a small header. Keep
      // cached app data and let the next loop-requested/manual cycle retry.
      owner->forceRefreshMask_ = 0;
      Serial.printf("[data] Central sync stopped after %s: Wi-Fi offline\n",
                    provider);
    }
    if (idle0 != nullptr) esp_task_wdt_add(idle0);
    // Scoped leases attempt to restore the reserve while provider-local result
    // and config objects still exist. Try once more after they are destroyed,
    // so the next queued provider starts with a full contiguous TLS arena.
    restoreTlsHeapGuard();
    if (owner->syncCompleted_ < owner->syncTotal_) {
      ++owner->syncCompleted_;
    }
    ++owner->providerJobs_;
    if (!providerOk) ++owner->providerFailures_;
    const uint32_t completedAt = millis();
    owner->lastProviderDurationMs_ =
        completedAt - owner->providerStartedAtMs_;
    const OnlineProvider completedProvider = owner->activeProvider_;
    const int8_t completedProviderIndex = providerIndex(completedProvider);
    if (completedProviderIndex >= 0) {
      volatile uint32_t& average =
          owner->providerAverageDurationMs_[completedProviderIndex];
      average = average == 0
                    ? owner->lastProviderDurationMs_
                    : (average * 3UL + owner->lastProviderDurationMs_) / 4UL;
      if (providerOk) {
        owner->consecutiveProviderFailures_[completedProviderIndex] = 0;
      } else {
        uint8_t& failures =
            owner->consecutiveProviderFailures_[completedProviderIndex];
        if (failures < UINT8_MAX) ++failures;
        if (failures >= kProviderFailureQuarantineThreshold) {
          owner->quarantineProvider(
              completedProvider,
              "Paused after three consecutive provider failures");
        }
      }
    }
    if (owner->lastProviderDurationMs_ >
        owner->maximumProviderDurationMs_) {
      owner->maximumProviderDurationMs_ = owner->lastProviderDurationMs_;
    }
    Serial.printf(
        "[data] %s complete in %lu ms; worker stack reserve: %u\n", provider,
        static_cast<unsigned long>(owner->lastProviderDurationMs_),
        static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    owner->activeProvider_ = OnlineProvider::None;
    if (linkLost || owner->forceRefreshMask_ == 0) {
      owner->syncActive_ = false;
      owner->lastSyncDurationMs_ = completedAt - owner->syncStartedAtMs_;
      owner->averageSyncDurationMs_ =
          owner->averageSyncDurationMs_ == 0
              ? owner->lastSyncDurationMs_
              : (owner->averageSyncDurationMs_ * 3UL +
                 owner->lastSyncDurationMs_) /
                    4UL;
      ++owner->syncCyclesCompleted_;
      if (linkLost) ++owner->syncCyclesAborted_;
      if (!linkLost) Serial.println(F("[data] Central sync complete"));
    }
    // Leave a guaranteed browser/local-app service window before the next
    // queued provider starts.
    owner->nextJobEarliestMs_ = completedAt + kProviderServiceGapMs;
    owner->taskRunning_ = false;
  }
}

bool DataServices::runFetch(const AppConfig& config, bool fetchRssData,
                            bool fetchNetatmoData, bool fetchMarketData,
                            bool fetchEnergyData, bool fetchCalendarData,
                            bool fetchTravelData, bool fetchRedditData,
                            bool fetchActivityData) {
  std::vector<Headline> headlines;
  std::vector<MarketQuote> marketQuotes;
  String rssError;
  String marketError;
  NetatmoReading netatmo;
  EnergyPriceReading energy;
  std::vector<EnergyPricePoint> energyPoints;
  std::vector<CalendarEvent> calendarEvents;
  std::vector<TravelStopSnapshot> travelStops;
  std::vector<Headline> redditPosts;
  String calendarError;
  String travelError;
  String redditError;
  ActivitySnapshot activity;
  bool rssOk = false;
  bool netatmoOk = false;
  bool marketOk = false;
  bool energyOk = false;
  bool calendarOk = false;
  bool travelOk = false;
  bool redditOk = false;
  bool activityOk = false;
  const std::vector<String> currentMarketSymbols =
      fetchMarketData ? configuredMarketSymbols(config)
                      : std::vector<String>();

  if (fetchRssData) {
    rssOk = fetchRss(config, headlines, rssError);
  }
  if (fetchNetatmoData) {
    AppConfig providerConfig = config;
    netatmo.moduleName.reserve(32);
    netatmo.error.reserve(128);
    ScopedTlsHeapLease tlsHeap;
    netatmoOk = fetchNetatmo(providerConfig, netatmo);
  }
  if (fetchMarketData) {
    marketOk = fetchMarket(config, marketQuotes, marketError);
  }
  if (fetchEnergyData) {
    ScopedTlsHeapLease tlsHeap;
    energyOk = fetchEnergy(config, energy, energyPoints);
  }
  if (fetchCalendarData) {
    ScopedTlsHeapLease tlsHeap;
    calendarOk = fetchCalendar(config, calendarEvents, calendarError);
  }
  if (fetchTravelData) {
    ScopedTlsHeapLease tlsHeap;
    travelOk = fetchTravel(config, travelStops, travelError);
  }
  if (fetchRedditData) {
    ScopedTlsHeapLease tlsHeap;
    redditOk = fetchReddit(config, redditPosts, redditError);
  }
  if (fetchActivityData) {
    AppConfig providerConfig = config;
    ScopedTlsHeapLease tlsHeap;
    activityOk = fetchGarminActivity(providerConfig, activity);
  }

  xSemaphoreTake(mutex_, portMAX_DELAY);
  if (fetchRssData) {
    if (rssOk) {
      if (!rssError.isEmpty() && !data_.headlines.empty()) {
        // Keep cached stories from an individual feed when the other feeds
        // refreshed successfully. A one-off TLS/provider failure should not
        // make BBC (or any other configured source) disappear for a full
        // refresh interval.
        for (const IconItem& feed : parseIconItems(config.rssFeedItems, 6)) {
          bool refreshed = false;
          for (const Headline& headline : headlines) {
            if (headline.sourceUrl == feed.value) {
              refreshed = true;
              break;
            }
          }
          if (refreshed) continue;
          uint8_t retained = 0;
          for (const Headline& old : data_.headlines) {
            if (old.sourceUrl == feed.value &&
                retained < config.rssHeadlineCount) {
              headlines.push_back(old);
              ++retained;
            }
          }
        }
      }
      if (!headlines.empty()) {
        if (headlines.size() > config.rssHeadlineCount) {
          headlines.resize(config.rssHeadlineCount);
        }
        uint32_t firstNewHeadlineId = 0;
        const bool firstSuccessfulLoad = recentHeadlineCount_ == 0;
        for (const Headline& headline : headlines) {
          bool seen = false;
          for (uint8_t index = 0; index < recentHeadlineCount_; ++index) {
            if (recentHeadlineIds_[index] == headline.id) {
              seen = true;
              break;
            }
          }
          if (seen) continue;
          if (firstNewHeadlineId == 0) firstNewHeadlineId = headline.id;
          if (recentHeadlineCount_ < kRecentHeadlineCapacity) {
            recentHeadlineIds_[recentHeadlineCount_++] = headline.id;
          } else {
            recentHeadlineIds_[recentHeadlineCursor_] = headline.id;
            recentHeadlineCursor_ =
                (recentHeadlineCursor_ + 1) % kRecentHeadlineCapacity;
          }
        }
        // The first load establishes the baseline silently. Afterwards a
        // headline can notify only once even if feed ordering changes or one
        // source temporarily disappears and returns. Retain the exact new ID
        // so the notification does not mistakenly reopen item zero.
        if (!firstSuccessfulLoad && firstNewHeadlineId != 0) {
          pendingHeadlineId_ = firstNewHeadlineId;
          newHeadlineEvent_ = true;
        }
      }
      data_.headlines = std::move(headlines);
      data_.rssUpdatedAt = time(nullptr);
      // Keep a failed feed visible without hiding headlines returned by the
      // healthy feeds.
      data_.rssError = rssError;
    } else {
      data_.rssError = rssError;
    }
  }
  if (fetchNetatmoData) {
    if (netatmoOk) {
      if (netatmo.rainAvailable && !isnan(netatmo.rainRateMmHour)) {
        const bool raining = netatmo.rainRateMmHour > 0.01f;
        if (config.climateRainAlertEnabled && previousRainStateKnown_ &&
            !previousRaining_ && raining &&
            (netatmo.rainMeasurementTime == 0 ||
             netatmo.rainMeasurementTime != lastRainEventMeasurementTime_)) {
          pendingRainReading_ = netatmo;
          rainStartEvent_ = true;
          lastRainEventMeasurementTime_ = netatmo.rainMeasurementTime;
        }
        // The first successful rain-gauge sample establishes a silent
        // baseline. This prevents a reboot during an existing shower from
        // replaying the same notification, while a later dry→wet transition
        // still emits exactly once.
        previousRaining_ = raining;
        previousRainStateKnown_ = true;
      }
      data_.netatmo = netatmo;
      data_.netatmoUpdatedAt = time(nullptr);
    } else if (!data_.netatmo.available) {
      data_.netatmo = netatmo;
    } else {
      // A transient HTTP/JSON problem must not erase the last valid outdoor
      // reading from the display.
      data_.netatmo.error = netatmo.error;
    }
  }
  if (fetchMarketData) {
    if (marketOk) {
      if (!marketError.isEmpty() && !data_.marketQuotes.empty()) {
        // Yahoo occasionally rejects one future while the other symbols
        // succeed. Retain only the missing symbol's last valid quote so the
        // slideshow still visits every configured market on that cycle.
        for (const MarketQuote& old : data_.marketQuotes) {
          if (!configuredMarketSymbol(currentMarketSymbols, old.symbol)) {
            continue;
          }
          bool refreshed = false;
          for (const MarketQuote& quote : marketQuotes) {
            if (quote.symbol == old.symbol) {
              refreshed = true;
              break;
            }
          }
          if (!refreshed) marketQuotes.push_back(old);
        }
      }
      data_.marketQuotes = std::move(marketQuotes);
      data_.marketUpdatedAt = time(nullptr);
      // A partial provider failure should remain visible even when another
      // configured symbol succeeded.
      data_.marketError = marketError;
    } else {
      // A removed ticker must disappear even if the provider fails during the
      // first refresh after the settings change. Retaining it indefinitely
      // makes a failed request look like a successful stale quote.
      data_.marketQuotes.erase(
          std::remove_if(data_.marketQuotes.begin(),
                         data_.marketQuotes.end(),
                         [&currentMarketSymbols](const MarketQuote& quote) {
                           return !configuredMarketSymbol(
                               currentMarketSymbols, quote.symbol);
                         }),
          data_.marketQuotes.end());
      data_.marketError = marketError;
    }
  }
  if (fetchEnergyData) {
    if (energyOk) {
      data_.energy = energy;
      data_.energyPoints = std::move(energyPoints);
      data_.energyChartRange = config.energyChartRange;
      data_.energyUpdatedAt = time(nullptr);
    } else if (!data_.energy.available) {
      data_.energy = energy;
    } else {
      data_.energy.error = energy.error;
    }
  }
  if (fetchCalendarData) {
    if (calendarOk) {
      data_.calendarEvents = std::move(calendarEvents);
      data_.calendarUpdatedAt = time(nullptr);
      data_.calendarError = calendarError;
    } else {
      data_.calendarError = calendarError;
    }
  }
  if (fetchTravelData) {
    if (travelOk) {
      data_.travelStops = std::move(travelStops);
      data_.travelUpdatedAt = time(nullptr);
      data_.travelError = travelError;
    } else {
      data_.travelError = travelError;
    }
  }
  if (fetchRedditData) {
    if (redditOk) {
      data_.redditPosts = std::move(redditPosts);
      data_.redditUpdatedAt = time(nullptr);
      data_.redditError = redditError;
    } else {
      data_.redditError = redditError;
    }
  }
  if (fetchActivityData) {
    if (activityOk) {
      data_.activity = activity;
      data_.activityUpdatedAt = activity.updatedAt;
      data_.activityError = "";
    } else if (!data_.activity.valid) {
      data_.activity = activity;
      data_.activityError = activity.error;
    } else {
      // The display keeps the last complete record. Only freshness and the
      // non-secret failure state change after a bad provider cycle.
      data_.activity.stale = true;
      data_.activity.reconnectRequired = activity.reconnectRequired;
      data_.activity.error = activity.error;
      data_.activityError = activity.error;
    }
  }
  xSemaphoreGive(mutex_);
  // Failed providers retain their last good snapshot and wait for the next
  // common cycle. Independent retry timers caused nearly continuous network
  // activity, made the web UI hard to reach, and refreshed different widgets
  // at surprising times.
  return fetchRssData       ? rssOk
         : fetchNetatmoData ? netatmoOk
         : fetchMarketData  ? marketOk
         : fetchEnergyData  ? energyOk
         : fetchCalendarData ? calendarOk
         : fetchTravelData ? travelOk
         : fetchRedditData  ? redditOk
         : fetchActivityData ? activityOk
                            : false;
}

bool DataServices::refreshGarminToken(AppConfig& config, String& error,
                                      bool& reconnectRequired) {
  error = "";
  reconnectRequired = false;
  if (config.garminClientId.isEmpty() ||
      config.garminRefreshToken.isEmpty()) {
    reconnectRequired = true;
    error = "Garmin credentials are incomplete";
    return false;
  }

  const String url = "https://diauth." + config.garminDomain +
                     "/di-oauth2-service/oauth/token";
  const String body =
      "grant_type=refresh_token&client_id=" +
      formEncode(config.garminClientId) + "&refresh_token=" +
      formEncode(config.garminRefreshToken);
  SafeTlsClient& client = resetProviderSecureClient();
  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);
  http.useHTTP10(true);
  if (!http.begin(client, url)) {
    error = "Garmin token connection could not start";
    return false;
  }
  addGarminNativeHeaders(http);
  http.addHeader("Authorization", basicAuthorization(config.garminClientId));
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Connection", "close");
  const int status = http.POST(body);
  String payload;
  String payloadError;
  const bool payloadOk =
      status >= 0 &&
      readBoundedHttpPayload(http, kMaximumGarminTokenResponseBytes,
                             "Garmin token", payload, payloadError);
  http.end();

  if (status < 200 || status >= 300) {
    reconnectRequired = status == 400 || status == 401 || status == 403 ||
                        payload.indexOf("invalid_grant") >= 0;
    if (status == 429) {
      error = "Garmin rate limit reached";
    } else if (reconnectRequired) {
      error = "Garmin connection expired; reconnect required";
    } else {
      error = "Garmin token " +
              (status < 0 ? HTTPClient::errorToString(status)
                          : String("HTTP ") + String(status));
    }
    garminAccessToken_ = "";
    return false;
  }
  if (!payloadOk) {
    error = payloadError;
    garminAccessToken_ = "";
    return false;
  }

  const char* accessTokenValue = nullptr;
  const char* refreshTokenValue = nullptr;
  const char* parseError = nullptr;
  if (!parseGarminTokenJson(payload.begin(), payload.length(),
                            accessTokenValue, refreshTokenValue,
                            parseError)) {
    error = "Garmin token JSON: " +
            String(parseError == nullptr ? "InvalidInput" : parseError);
    garminAccessToken_ = "";
    return false;
  }

  // Copy only the two retained values after the in-place decode has
  // succeeded. This replaces a 10 KiB DynamicJsonDocument allocation that
  // could fail on a healthy but fragmented ESP32 heap with `NoMemory`.
  const String accessToken(accessTokenValue == nullptr ? ""
                                                       : accessTokenValue);
  const String rotatedRefresh(refreshTokenValue == nullptr
                                  ? ""
                                  : refreshTokenValue);
  if (accessToken.isEmpty() ||
      accessToken.length() > kMaximumGarminAccessTokenBytes) {
    reconnectRequired = true;
    error = "Garmin did not return a valid access token";
    garminAccessToken_ = "";
    return false;
  }
  if (rotatedRefresh.length() > kMaximumGarminRefreshTokenBytes) {
    reconnectRequired = true;
    error = "Garmin returned an invalid refresh token";
    garminAccessToken_ = "";
    return false;
  }

  if (!rotatedRefresh.isEmpty() &&
      rotatedRefresh != config.garminRefreshToken) {
    if (!configStore_.updateGarminCredentials(
            config.garminDomain, config.garminClientId, rotatedRefresh)) {
      error = "Garmin token could not be stored";
      garminAccessToken_ = "";
      return false;
    }
    config.garminRefreshToken = rotatedRefresh;
  }
  // Access tokens deliberately never enter AppConfig or persistent storage.
  garminAccessToken_ = accessToken;
  return true;
}

bool DataServices::fetchGarminJson(
    AppConfig& config, const String& path, String& payload, String& error,
    bool& reconnectRequired,
    bool& refreshedAfterAuthorizationError) {
  payload = "";
  error = "";
  reconnectRequired = false;
  if (garminAccessToken_.isEmpty() &&
      !refreshGarminToken(config, error, reconnectRequired)) {
    return false;
  }

  size_t maximumBytes = kMaximumGarminSummaryBytes;
  if (path.startsWith("/userprofile-service/")) {
    maximumBytes = kMaximumGarminProfileBytes;
  } else if (path.startsWith("/activitylist-service/")) {
    maximumBytes = kMaximumGarminActivitiesBytes;
  }

  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    SafeTlsClient& client = resetProviderSecureClient();
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    http.setReuse(false);
    http.useHTTP10(true);
    const String url = "https://connectapi." + config.garminDomain + path;
    if (!http.begin(client, url)) {
      error = "Garmin API connection could not start";
      return false;
    }
    addGarminNativeHeaders(http);
    http.addHeader("Authorization", "Bearer " + garminAccessToken_);
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.addHeader("Connection", "close");
    const int status = http.GET();
    if (status == 401 && !refreshedAfterAuthorizationError) {
      http.end();
      garminAccessToken_ = "";
      if (!refreshGarminToken(config, error, reconnectRequired)) {
        return false;
      }
      refreshedAfterAuthorizationError = true;
      continue;
    }
    if (status < 200 || status >= 300) {
      http.end();
      if (status == 401) {
        reconnectRequired = true;
        error = "Garmin connection expired; reconnect required";
        garminAccessToken_ = "";
      } else if (status == 429) {
        error = "Garmin rate limit reached";
      } else {
        error = "Garmin API " +
                (status < 0 ? HTTPClient::errorToString(status)
                            : String("HTTP ") + String(status));
      }
      return false;
    }
    const bool ok = readBoundedHttpPayload(
        http, maximumBytes, "Garmin API", payload, error);
    http.end();
    return ok;
  }
  error = "Garmin authorization retry failed";
  return false;
}

bool DataServices::fetchGarminActivity(AppConfig& config,
                                       ActivitySnapshot& activity) {
  activity = ActivitySnapshot{};
  activity.stale = true;
  if (config.garminClientId.isEmpty() ||
      config.garminRefreshToken.isEmpty()) {
    activity.reconnectRequired = true;
    activity.error = "Garmin credentials are incomplete";
    return false;
  }

  String displayName = config.garminDisplayName;
  displayName.trim();
  if (displayName.isEmpty()) displayName = garminResolvedDisplayName_;
  if (displayName.isEmpty()) {
    String payload;
    String error;
    bool reconnectRequired = false;
    bool refreshedAfterAuthorizationError = false;
    if (!fetchGarminJson(config, "/userprofile-service/socialProfile",
                         payload, error, reconnectRequired,
                         refreshedAfterAuthorizationError)) {
      activity.reconnectRequired = reconnectRequired;
      activity.error = error;
      return false;
    }
    StaticJsonDocument<96> filter;
    filter["displayName"] = true;
    filter["userName"] = true;
    DynamicJsonDocument document(1024);
    const DeserializationError parseError =
        deserializeJson(document, payload,
                        DeserializationOption::Filter(filter));
    if (parseError) {
      activity.error = "Garmin profile JSON: " + String(parseError.c_str());
      return false;
    }
    displayName = document["displayName"].as<String>();
    if (displayName.isEmpty()) {
      displayName = document["userName"].as<String>();
    }
    displayName.trim();
    truncateUtf8(displayName, 128);
    if (displayName.isEmpty()) {
      activity.error = "Garmin profile has no display name";
      return false;
    }
    garminResolvedDisplayName_ = displayName;
  }

  const time_t now = time(nullptr);
  if (now <= 1672531200) {
    activity.error = "Garmin needs a synchronized clock";
    return false;
  }
  tm local{};
  localtime_r(&now, &local);
  char date[11];
  strftime(date, sizeof(date), "%Y-%m-%d", &local);

  {
    const String path =
        "/usersummary-service/usersummary/daily/" +
        formEncode(displayName) + "?calendarDate=" + String(date);
    String payload;
    String error;
    bool reconnectRequired = false;
    bool refreshedAfterAuthorizationError = false;
    if (!fetchGarminJson(config, path, payload, error, reconnectRequired,
                         refreshedAfterAuthorizationError)) {
      activity.reconnectRequired = reconnectRequired;
      activity.error = error;
      return false;
    }
    StaticJsonDocument<128> filter;
    filter["totalSteps"] = true;
    filter["totalDistanceMeters"] = true;
    filter["totalDistance"] = true;
    DynamicJsonDocument document(1024);
    const DeserializationError parseError =
        deserializeJson(document, payload,
                        DeserializationOption::Filter(filter));
    if (parseError || !document.is<JsonObject>()) {
      activity.error = "Garmin daily summary JSON: " +
                       String(parseError.c_str());
      return false;
    }
    activity.stepsToday = boundedGarminNumber(document["totalSteps"]);
    activity.distanceTodayM =
        boundedGarminNumber(document["totalDistanceMeters"]);
    if (activity.distanceTodayM == 0) {
      activity.distanceTodayM =
          boundedGarminNumber(document["totalDistance"]);
    }
  }

  {
    String payload;
    String error;
    bool reconnectRequired = false;
    bool refreshedAfterAuthorizationError = false;
    if (!fetchGarminJson(
            config,
            "/activitylist-service/activities/search/activities?start=0&limit=1",
            payload, error, reconnectRequired,
            refreshedAfterAuthorizationError)) {
      activity.reconnectRequired = reconnectRequired;
      activity.error = error;
      return false;
    }
    StaticJsonDocument<384> filter;
    JsonObject first = filter[0].to<JsonObject>();
    first["activityName"] = true;
    first["activityType"]["typeKey"] = true;
    first["distance"] = true;
    first["duration"] = true;
    first["calories"] = true;
    first["beginTimestamp"] = true;
    first["startTimeGMT"] = true;
    DynamicJsonDocument document(2048);
    const DeserializationError parseError =
        deserializeJson(document, payload,
                        DeserializationOption::Filter(filter));
    if (parseError || !document.is<JsonArray>()) {
      activity.error = "Garmin activities JSON: " +
                       String(parseError.c_str());
      return false;
    }
    JsonObjectConst latest = document[0].as<JsonObjectConst>();
    if (!latest.isNull()) {
      activity.latestName = latest["activityName"].as<String>();
      truncateUtf8(activity.latestName, 96);
      const String typeKey =
          latest["activityType"]["typeKey"].as<String>();
      activity.latestType = garminActivityTypeFromKey(typeKey.c_str());
      activity.latestDistanceM = boundedGarminNumber(latest["distance"]);
      activity.latestDurationSec = boundedGarminNumber(latest["duration"]);
      activity.latestCaloriesKcal = static_cast<uint16_t>(min<uint32_t>(
          UINT16_MAX, boundedGarminNumber(latest["calories"])));
      activity.latestStartedAt = parseGarminStart(latest);
    }
  }

  activity.valid = true;
  activity.stale = false;
  activity.reconnectRequired = false;
  activity.updatedAt = static_cast<uint32_t>(now);
  activity.error = "";
  return true;
}

bool DataServices::fetchRss(const AppConfig& config,
                            std::vector<Headline>& headlines,
                            String& error) {
  bool any = false;
  const std::vector<IconItem> configuredFeeds =
      parseIconItems(config.rssFeedItems, 6);
  const uint8_t perFeed = max<uint8_t>(
      1, (config.rssHeadlineCount + configuredFeeds.size() - 1) /
             max<size_t>(1, configuredFeeds.size()));

  // Reserve the final round-robin result before any feed handshakes. After
  // several TLS sessions the general heap can have enough free bytes but no
  // contiguous block for a vector growth; allocating this small fixed table
  // up front prevents std::vector's throwing allocator from rebooting ESP32.
  headlines.resize(config.rssHeadlineCount);

  // Allocate every retained headline slot before the first TLS lease. If the
  // next feed's vector is allocated between handshakes it can split the heap
  // guard that was just restored, leaving a later source (typically the last
  // one, such as BBC) without one large enough mbedTLS block.
  std::vector<std::vector<Headline>> feeds(configuredFeeds.size());
  std::vector<uint8_t> feedSucceeded(configuredFeeds.size(), 0);
  for (size_t feedIndex = 0; feedIndex < configuredFeeds.size(); ++feedIndex) {
    const IconItem& feed = configuredFeeds[feedIndex];
    const String source = sourceFromUrl(feed.value);
    feeds[feedIndex].resize(perFeed);
    for (Headline& headline : feeds[feedIndex]) {
      headline.source.reserve(max<size_t>(source.length(), 12));
      headline.sourceUrl.reserve(feed.value.length());
      headline.title.reserve(config.maximumMessageChars + 4U);
    }
  }

  for (size_t feedIndex = 0; feedIndex < configuredFeeds.size(); ++feedIndex) {
    const IconItem& feed = configuredFeeds[feedIndex];
    String feedError;
    const String source = sourceFromUrl(feed.value);
    bool ok = false;
    {
      ScopedTlsHeapLease tlsHeap;
      ok = fetchRssUrl(feed.value, source, perFeed,
                       config.maximumMessageChars, feeds[feedIndex], feedError);
    }
    any |= ok;
    feedSucceeded[feedIndex] = ok ? 1 : 0;
    if (!feedError.isEmpty()) {
      if (!error.isEmpty()) {
        error += "; ";
      }
      error += feedError;
    }
  }
  // Round-robin feeds instead of appending an entire source at a time. A
  // short slideshow visit now reaches BBC (and every other healthy feed)
  // within one cycle instead of after dozens of earlier-source headlines.
  size_t headlineCount = 0;
  for (uint8_t item = 0;
       item < perFeed && headlineCount < config.rssHeadlineCount; ++item) {
    for (size_t feedIndex = 0; feedIndex < feeds.size(); ++feedIndex) {
      auto& feed = feeds[feedIndex];
      if (feedSucceeded[feedIndex] && item < feed.size()) {
        headlines[headlineCount++] = std::move(feed[item]);
      }
      if (headlineCount >= config.rssHeadlineCount) break;
    }
  }
  headlines.resize(headlineCount);
  return any && !headlines.empty();
}

bool DataServices::fetchRssUrl(const String& url, const String& source,
                               uint8_t maximumItems, uint16_t maximumChars,
                               std::vector<Headline>& headlines,
                               String& error,
                               const std::vector<IconItem>* dynamicSources) {
  String requestUrl = url;
  for (uint8_t redirect = 0; redirect < 5; ++redirect) {
    if (WiFi.status() != WL_CONNECTED) {
      error = source + ": Wi-Fi offline";
      return false;
    }
    WiFiClient plainClient;
    SafeTlsClient& secureClient = resetProviderSecureClient();
    WiFiClient* client = &plainClient;
    if (requestUrl.startsWith("https://")) {
      // The full embedded public CA bundle authenticates user-configurable
      // HTTPS feeds as well as PikOS's fixed provider endpoints.
      client = &secureClient;
    } else if (!requestUrl.startsWith("http://")) {
      error = source + ": invalid URL";
      return false;
    }
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    http.setUserAgent(kUserAgent);
    if (!http.begin(*client, requestUrl)) {
      error = source + ": invalid URL";
      return false;
    }
    const int status = http.GET();
    if (isRedirectStatus(status)) {
      const String next = resolveRedirect(requestUrl, http.getLocation());
      http.end();
      if (next.isEmpty()) {
        error = source + ": invalid redirect";
        return false;
      }
      requestUrl = next;
      continue;
    }
    // The ESP32 TLS stack occasionally reports a connection failure while
    // the same feed succeeds moments later. Recreate the whole client on the
    // next loop iteration; the retry budget is shared with redirects so a
    // broken endpoint can never monopolize the online-data task.
    if ((status < 0 || status >= 500) && redirect < 4) {
      http.end();
      delay(180UL * (redirect + 1));
      continue;
    }
    if (status < 200 || status >= 300) {
      error = source + ": HTTP " + String(status);
      http.end();
      return false;
    }

    FeedTitleParser parser(source, url, maximumItems, maximumChars,
                           headlines, dynamicSources);
    WiFiClient* stream = http.getStreamPtr();
    size_t total = 0;
    uint32_t lastDataMs = millis();
    while ((http.connected() || stream->available() > 0) &&
           total < kMaximumRssBytes && !parser.full()) {
      const int available = stream->available();
      if (available <= 0) {
        if (millis() - lastDataMs > kHttpTimeoutMs) {
          break;
        }
        delay(1);
        continue;
      }
      const int count = min(available, 512);
      for (int i = 0; i < count && total < kMaximumRssBytes; ++i) {
        parser.feed(static_cast<char>(stream->read()));
        ++total;
      }
      lastDataMs = millis();
      vTaskDelay(1);
    }
    http.end();
    const size_t parsedItems = parser.count();
    headlines.resize(parsedItems);
    if (parsedItems == 0) {
      error = source + ": no RSS/Atom titles found";
      return false;
    }
    return true;
  }
  error = source + ": too many redirects";
  return false;
}

bool DataServices::refreshNetatmoToken(AppConfig& config, String& error) {
  if (config.netatmoClientId.isEmpty() ||
      config.netatmoClientSecret.isEmpty() ||
      config.netatmoRefreshToken.isEmpty()) {
    error = "Netatmo refresh credentials are incomplete";
    return false;
  }

  const String body =
      "grant_type=refresh_token&refresh_token=" +
      formEncode(config.netatmoRefreshToken) +
      "&client_id=" + formEncode(config.netatmoClientId) +
      "&client_secret=" + formEncode(config.netatmoClientSecret);
  String payload;
  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    SafeTlsClient& client = resetProviderSecureClient();
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    http.setReuse(false);
    http.useHTTP10(true);
    if (!http.begin(client, "https://api.netatmo.com/oauth2/token")) {
      error = "Netatmo token connection could not start";
      if (attempt < 2) {
        delay(250UL * (attempt + 1));
        continue;
      }
      return false;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.addHeader("Connection", "close");
    const int status = http.POST(body);
    if (status >= 200 && status < 300) {
      payload = http.getString();
      http.end();
      break;
    }
    const String detail =
        status < 0 ? HTTPClient::errorToString(status) : String(status);
    error = "Netatmo token connection: " + detail;
    http.end();
    const bool transient = status < 0 || status >= 500;
    if (!transient || attempt == 2) {
      return false;
    }
    delay(250UL * (attempt + 1));
  }
  if (payload.isEmpty()) {
    error = "Netatmo token response was empty";
    return false;
  }
  DynamicJsonDocument document(4096);
  const DeserializationError parseError =
      deserializeJson(document, payload);
  if (parseError) {
    error = "Netatmo token JSON: " + String(parseError.c_str()) +
            " (" + String(payload.length()) + " bytes)";
    return false;
  }
  config.netatmoAccessToken =
      document["access_token"].as<String>();
  const String newRefresh = document["refresh_token"].as<String>();
  if (!newRefresh.isEmpty()) {
    config.netatmoRefreshToken = newRefresh;
  }
  if (config.netatmoAccessToken.isEmpty()) {
    error = "Netatmo did not return an access token";
    return false;
  }
  configStore_.updateNetatmoTokens(config.netatmoAccessToken,
                                    config.netatmoRefreshToken);
  return true;
}

bool DataServices::fetchNetatmo(AppConfig& config,
                                NetatmoReading& reading) {
  if (config.netatmoAccessToken.isEmpty()) {
    if (!refreshNetatmoToken(config, reading.error)) {
      return false;
    }
  }

  bool refreshedAfterAuthorizationError = false;
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    SafeTlsClient& client = resetProviderSecureClient();
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    http.setReuse(false);
    http.useHTTP10(true);
    String url = "https://api.netatmo.com/api/getstationsdata";
    if (!config.netatmoDeviceId.isEmpty()) {
      url += "?device_id=" + formEncode(config.netatmoDeviceId);
    }
    if (!http.begin(client, url)) {
      reading.error = "Netatmo URL failed";
      return false;
    }
    http.addHeader("Authorization",
                   "Bearer " + config.netatmoAccessToken);
    http.addHeader("Accept", "application/json");
    http.addHeader("Connection", "close");
    const int status = http.GET();
    if ((status == 401 || status == 403) &&
        !refreshedAfterAuthorizationError) {
      http.end();
      if (!refreshNetatmoToken(config, reading.error)) {
        return false;
      }
      refreshedAfterAuthorizationError = true;
      continue;
    }
    if ((status < 0 || status >= 500) && attempt < 3) {
      reading.error =
          "Netatmo connection: " +
          (status < 0 ? HTTPClient::errorToString(status)
                      : String(status));
      http.end();
      delay(250UL * (attempt + 1));
      continue;
    }
    if (status < 200 || status >= 300) {
      reading.error =
          "Netatmo " +
          (status < 0 ? HTTPClient::errorToString(status)
                      : String("HTTP ") + String(status));
      http.end();
      return false;
    }

    // Buffer this response before parsing. Some Netatmo accounts return a
    // large station document in several TLS records; parsing directly from
    // HTTPClient's stream could see a temporary empty read as end-of-input.
    const String payload = http.getString();
    http.end();
    if (payload.isEmpty()) {
      reading.error = "Netatmo returned an empty response";
      return false;
    }
    size_t firstContent = 0;
    while (firstContent < payload.length() &&
           isspace(static_cast<unsigned char>(payload[firstContent]))) {
      ++firstContent;
    }
    if (firstContent >= payload.length() || payload[firstContent] != '{') {
      reading.error = "Netatmo returned a non-JSON response";
      return false;
    }

    // Netatmo station documents can be large. Retain only the identity and
    // dashboard fields used by PikOS to avoid heap fragmentation and brief
    // NoMemory errors.
    StaticJsonDocument<1536> filter;
    JsonObject stationFilter = filter["body"]["devices"][0].to<JsonObject>();
    stationFilter["dashboard_data"]["Pressure"] = true;
    JsonObject moduleFilter =
        stationFilter["modules"][0].to<JsonObject>();
    moduleFilter["_id"] = true;
    moduleFilter["module_name"] = true;
    moduleFilter["type"] = true;
    moduleFilter["dashboard_data"]["Temperature"] = true;
    moduleFilter["dashboard_data"]["Humidity"] = true;
    moduleFilter["dashboard_data"]["CO2"] = true;
    moduleFilter["dashboard_data"]["Pressure"] = true;
    moduleFilter["dashboard_data"]["Rain"] = true;
    moduleFilter["dashboard_data"]["sum_rain_1"] = true;
    moduleFilter["dashboard_data"]["sum_rain_24"] = true;
    moduleFilter["dashboard_data"]["WindStrength"] = true;
    moduleFilter["dashboard_data"]["WindAngle"] = true;
    moduleFilter["dashboard_data"]["GustStrength"] = true;
    moduleFilter["dashboard_data"]["GustAngle"] = true;
    moduleFilter["dashboard_data"]["max_wind_str"] = true;
    moduleFilter["dashboard_data"]["max_wind_angle"] = true;
    moduleFilter["dashboard_data"]["date_max_wind_str"] = true;
    moduleFilter["dashboard_data"]["time_utc"] = true;

    // The filter retains roughly 2 KiB even for a much larger station
    // response. Reserving 24 KiB needlessly competed with the TLS arena and
    // could make ArduinoJson report NoMemory after the RSS cache was filled.
    DynamicJsonDocument document(6144);
    const DeserializationError parseError =
        deserializeJson(document, payload,
                        DeserializationOption::Filter(filter));
    if (parseError) {
      reading.error = "Netatmo JSON: " + String(parseError.c_str()) +
                      " (" + String(payload.length()) + " bytes)";
      return false;
    }

    JsonObject station = document["body"]["devices"][0];
    if (station.isNull()) {
      reading.error = "No Netatmo station returned";
      return false;
    }
    JsonObject selected;
    JsonObject outdoor;
    JsonObject rainModule;
    JsonObject windModule;
    JsonObject fallback;
    for (JsonObject module : station["modules"].as<JsonArray>()) {
      const String id = module["_id"].as<String>();
      const String name = module["module_name"].as<String>();
      const String type = module["type"].as<String>();
      if (fallback.isNull()) {
        fallback = module;
      }
      if (!config.netatmoModuleId.isEmpty() &&
          id == config.netatmoModuleId) {
        selected = module;
      }
      if (config.netatmoModuleId.isEmpty() &&
          !config.netatmoModuleName.isEmpty() &&
          name.equalsIgnoreCase(config.netatmoModuleName)) {
        selected = module;
      }
      if (outdoor.isNull() && type == "NAModule1") {
        outdoor = module;
      }
      if (rainModule.isNull() && type == "NAModule3") {
        rainModule = module;
      }
      if (windModule.isNull() && type == "NAModule2") {
        windModule = module;
      }
    }
    if (selected.isNull()) {
      if (!config.netatmoModuleId.isEmpty()) {
        reading.error = "Configured Netatmo module was not found";
        return false;
      }
      // A friendly module name is only a preference. If it was renamed or
      // the station name was entered by mistake, still choose the outdoor
      // NAModule1 rather than hiding otherwise valid measurements.
      selected = outdoor.isNull() ? fallback : outdoor;
    }
    if (selected.isNull()) {
      reading.error = "No Netatmo outdoor module returned";
      return false;
    }
    JsonObject dashboard = selected["dashboard_data"];
    JsonObject stationDashboard = station["dashboard_data"];
    if (dashboard.isNull()) {
      reading.error = "Netatmo module has no dashboard data";
      return false;
    }

    reading.available = true;
    reading.moduleName = selected["module_name"] | String("Netatmo");
    reading.temperatureC = jsonFloatOrNan(dashboard["Temperature"]);
    reading.humidityPercent = jsonFloatOrNan(dashboard["Humidity"]);
    reading.co2Ppm = jsonFloatOrNan(dashboard["CO2"]);
    reading.pressureHpa = jsonFloatOrNan(dashboard["Pressure"]);
    if (isnan(reading.pressureHpa)) {
      reading.pressureHpa =
          jsonFloatOrNan(stationDashboard["Pressure"]);
    }
    reading.measurementTime = dashboard["time_utc"] | 0;
    if (!rainModule.isNull()) {
      JsonObject rain = rainModule["dashboard_data"];
      reading.rainRateMmHour = jsonFloatOrNan(rain["Rain"]);
      reading.rainLastHourMm = jsonFloatOrNan(rain["sum_rain_1"]);
      reading.rainLast24HoursMm = jsonFloatOrNan(rain["sum_rain_24"]);
      reading.rainMeasurementTime = rain["time_utc"] | 0;
      reading.rainAvailable =
          !isnan(reading.rainRateMmHour) ||
          !isnan(reading.rainLastHourMm) ||
          !isnan(reading.rainLast24HoursMm);
    }
    if (!windModule.isNull()) {
      JsonObject wind = windModule["dashboard_data"];
      reading.windSpeedKph = jsonFloatOrNan(wind["WindStrength"]);
      reading.windGustKph = jsonFloatOrNan(wind["GustStrength"]);
      reading.windDirectionDegrees =
          wind["WindAngle"].isNull()
              ? -1
              : static_cast<int16_t>(wind["WindAngle"].as<int>());
      reading.windGustDirectionDegrees =
          wind["GustAngle"].isNull()
              ? -1
              : static_cast<int16_t>(wind["GustAngle"].as<int>());
      reading.windMeasurementTime = wind["time_utc"] | 0;
      reading.windMaximumKph = jsonFloatOrNan(wind["max_wind_str"]);
      reading.windMaximumDirectionDegrees =
          wind["max_wind_angle"].isNull()
              ? -1
              : static_cast<int16_t>(wind["max_wind_angle"].as<int>());
      reading.windMaximumTime = wind["date_max_wind_str"] | 0;
      reading.windAvailable = !isnan(reading.windSpeedKph) ||
                              !isnan(reading.windGustKph);
    }
    reading.error = "";
    return true;
  }
  if (reading.error.isEmpty()) {
    reading.error = "Netatmo retry limit reached";
  }
  return false;
}

bool DataServices::fetchMarket(const AppConfig& config,
                               std::vector<MarketQuote>& quotes,
                               String& error) {
  const MarketWindow window = marketWindow(config.marketChartRange);
  // Be defensive with configurations made by older PikOS versions, where a
  // display currency prefix was accidentally stored as part of the ticker.
  std::vector<String> symbols = configuredMarketSymbols(config);
  if (symbols.empty()) {
    error = "No market symbols configured";
    return false;
  }

  if (config.marketProvider != "twelve") {
    String lastError;
    // Reuse the small filter definition, but allocate each result document
    // only after its TLS request is established. Keeping a 12 KiB document
    // alive during the next ESP32 TLS handshake can starve the connection.
    StaticJsonDocument<768> yahooFilter;
    JsonObject yahooResultFilter =
        yahooFilter["chart"]["result"][0].to<JsonObject>();
    yahooResultFilter["meta"]["regularMarketPrice"] = true;
    yahooResultFilter["meta"]["chartPreviousClose"] = true;
    yahooResultFilter["meta"]["previousClose"] = true;
    yahooResultFilter["meta"]["shortName"] = true;
    yahooResultFilter["meta"]["currency"] = true;
    yahooResultFilter["meta"]["marketState"] = true;
    yahooResultFilter["indicators"]["quote"][0]["close"] = true;
    yahooFilter["chart"]["error"]["description"] = true;
    for (const String& displaySymbol : symbols) {
      ScopedTlsHeapLease symbolTlsHeap;
      String yahooSymbol = displaySymbol;
      yahooSymbol.trim();
      MarketQuote cryptoQuote;
      String cryptoError;
      if (fetchKrakenCryptoPair(displaySymbol, cryptoQuote, window,
                                cryptoError)) {
        quotes.push_back(cryptoQuote);
        continue;
      }
      if (!cryptoError.isEmpty()) {
        lastError = cryptoError;
      }
      const int slash = yahooSymbol.indexOf('/');
      if (slash >= 0) {
        MarketQuote forexQuote;
        String forexError;
        if (fetchFrankfurterPair(displaySymbol, forexQuote, window,
                                 forexError)) {
          quotes.push_back(forexQuote);
          continue;
        }
        lastError = forexError;
        String base = yahooSymbol.substring(0, slash);
        String quote = yahooSymbol.substring(slash + 1);
        base.toUpperCase();
        quote.toUpperCase();
        // Yahoo's canonical shorthand for USD/GBP is GBP=X (similarly
        // EUR=X and JPY=X). Other crosses use the concatenated pair.
        yahooSymbol =
            base == "USD" ? quote + "=X" : base + quote + "=X";
      }

      String encodedYahooSymbol = formEncode(yahooSymbol);
      // Yahoo accepts escaped punctuation in the path but its chart route
      // treats an escaped equals sign as an invalid ticker.
      encodedYahooSymbol.replace("%3D", "=");
      const String url =
          "https://query1.finance.yahoo.com/v8/finance/chart/" +
          encodedYahooSymbol + "?interval=" + window.interval +
          "&range=" + window.range;
      // Futures responses can contain large timezone/trading-period tables.
      // Keep only the quote metadata and close series PikOS renders; parsing
      // the whole Yahoo document caused NQ=F to intermittently fail with
      // ArduinoJson NoMemory on the ESP32.
      bool yahooParsed = false;
      String yahooFailure = "Yahoo request failed";
      std::unique_ptr<DynamicJsonDocument> yahooDocument;
      for (uint8_t attempt = 0; attempt < 3 && !yahooParsed; ++attempt) {
        SafeTlsClient& client = resetProviderSecureClient();
        HTTPClient http;
        http.setConnectTimeout(kHttpTimeoutMs);
        http.setTimeout(kHttpTimeoutMs);
        if (!http.begin(client, url)) {
          yahooFailure = "Yahoo URL failed";
        } else {
          http.setUserAgent(String("Mozilla/5.0 ") + kUserAgent);
          http.addHeader("Accept", "application/json");
          const int status = http.GET();
          if (status >= 200 && status < 300) {
            yahooDocument.reset(new DynamicJsonDocument(8192));
            const DeserializationError parseError = deserializeJson(
                *yahooDocument, http.getStream(),
                DeserializationOption::Filter(yahooFilter));
            if (!parseError) {
              yahooParsed = true;
            } else {
              yahooFailure = "Yahoo JSON " + String(parseError.c_str());
              yahooDocument.reset();
            }
          } else {
            yahooFailure = "Yahoo HTTP " + String(status);
          }
          http.end();
        }
        if (!yahooParsed && attempt < 2) {
          delay(220UL * (attempt + 1));
        }
      }
      if (!yahooParsed) {
        lastError = displaySymbol + ": " + yahooFailure;
        continue;
      }

      JsonObject result = (*yahooDocument)["chart"]["result"][0];
      if (result.isNull()) {
        const String description =
            (*yahooDocument)["chart"]["error"]["description"].as<String>();
        if (description.isEmpty() || description == "null") {
          lastError = displaySymbol + ": Yahoo returned no quote";
        } else {
          lastError = displaySymbol + ": " + description;
        }
        continue;
      }
      JsonObject meta = result["meta"];
      float price = jsonFloatOrNan(meta["regularMarketPrice"]);
      float previous = jsonFloatOrNan(meta["chartPreviousClose"]);
      if (isnan(previous)) {
        previous = jsonFloatOrNan(meta["previousClose"]);
      }

      std::vector<float> rawHistory;
      float penultimate = NAN;
      float latest = NAN;
      for (JsonVariant value :
           result["indicators"]["quote"][0]["close"].as<JsonArray>()) {
        if (!value.isNull()) {
          penultimate = latest;
          latest = value.as<float>();
          rawHistory.push_back(latest);
        }
      }
      if (isnan(price)) {
        price = latest;
      }
      if (isnan(previous)) {
        previous = penultimate;
      }
      if (rawHistory.size() == 1 && !isnan(previous)) {
        rawHistory.insert(rawHistory.begin(), previous);
      }
      if (rawHistory.empty() && !isnan(previous) && !isnan(price)) {
        rawHistory.push_back(previous);
        rawHistory.push_back(price);
      } else if (!rawHistory.empty() && !isnan(price)) {
        rawHistory.back() = price;
      }
      if (isnan(price)) {
        lastError = displaySymbol + ": Yahoo returned no price";
        continue;
      }

      MarketQuote quote;
      quote.available = true;
      quote.symbol = displaySymbol;
      quote.name = meta["shortName"] | displaySymbol;
      quote.currency = meta["currency"] | String("");
      quote.price = price;
      quote.percentChange =
          !isnan(previous) && previous != 0.0f
              ? (price - previous) / previous * 100.0f
              : NAN;
      quote.marketOpen =
          meta["marketState"].as<String>() == "REGULAR";
      quote.sparklineCount =
          min<size_t>(23, rawHistory.size());
      for (uint8_t bin = 0; bin < quote.sparklineCount; ++bin) {
        const size_t first =
            bin * rawHistory.size() / quote.sparklineCount;
        const size_t end = max<size_t>(
            first + 1,
            (bin + 1) * rawHistory.size() / quote.sparklineCount);
        float total = 0.0f;
        const size_t boundedEnd = min<size_t>(end, rawHistory.size());
        for (size_t index = first; index < boundedEnd; ++index) {
          total += rawHistory[index];
        }
        quote.sparkline[bin] =
            total / static_cast<float>(boundedEnd - first);
      }
      quotes.push_back(quote);
    }
    if (quotes.empty()) {
      error = lastError.isEmpty() ? "Yahoo returned no quotes" : lastError;
      return false;
    }
    error = lastError;
    return true;
  }

  if (config.marketApiKey.isEmpty()) {
    error = "Twelve Data API key missing";
    return false;
  }

  String joined;
  for (const String& symbol : symbols) {
    if (!joined.isEmpty()) {
      joined += ",";
    }
    joined += symbol;
  }

  SafeTlsClient& client = resetProviderSecureClient();
  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  const String url =
      "https://api.twelvedata.com/quote?symbol=" + formEncode(joined);
  if (!http.begin(client, url)) {
    error = "Twelve Data URL failed";
    return false;
  }
  http.addHeader("Authorization", "apikey " + config.marketApiKey);
  http.addHeader("Accept", "application/json");
  const int status = http.GET();
  if (status < 200 || status >= 300) {
    error = "Twelve Data HTTP " + String(status);
    http.end();
    return false;
  }

  DynamicJsonDocument document(32768);
  const DeserializationError parseError =
      deserializeJson(document, http.getStream());
  http.end();
  if (parseError) {
    error = "Twelve Data JSON: " + String(parseError.c_str());
    return false;
  }
  if (document["status"].as<String>() == "error") {
    error = document["message"] | String("Twelve Data request failed");
    return false;
  }

  auto addQuote = [&quotes](JsonObject object,
                            const String& fallbackSymbol) -> bool {
    if (object.isNull() || object["status"].as<String>() == "error") {
      return false;
    }
    MarketQuote quote;
    quote.symbol = object["symbol"] | fallbackSymbol;
    quote.name = object["name"] | quote.symbol;
    quote.currency = object["currency"] | String("");
    quote.price = jsonFloatOrNan(object["close"]);
    if (isnan(quote.price)) {
      quote.price = jsonFloatOrNan(object["price"]);
    }
    quote.percentChange = jsonFloatOrNan(object["percent_change"]);
    quote.marketOpen = object["is_market_open"] | false;
    quote.available = !isnan(quote.price);
    if (quote.available) {
      quotes.push_back(quote);
      return true;
    }
    return false;
  };

  if (symbols.size() == 1 && !document["symbol"].isNull()) {
    addQuote(document.as<JsonObject>(), symbols.front());
  } else {
    for (const String& symbol : symbols) {
      addQuote(document[symbol].as<JsonObject>(), symbol);
    }
  }
  if (quotes.empty()) {
    error = "Twelve Data returned no quotes";
    return false;
  }
  return true;
}


bool DataServices::fetchCalendar(const AppConfig& config,
                                 std::vector<CalendarEvent>& events,
                                 String& error) {
  std::vector<CalendarItem> items = parseCalendarItems(config.calendarItems, 16);
  if (items.empty()) {
    const String urls[] = {config.calendarIcalUrl, config.calendarIcalUrl2,
                           config.calendarIcalUrl3, config.calendarIcalUrl4};
    const String names[] = {config.calendarName1, config.calendarName2,
                            config.calendarName3, config.calendarName4};
    for (uint8_t index = 0; index < 4; ++index) {
      if (!urls[index].isEmpty()) {
        items.push_back({names[index], urls[index], config.calendarIconId});
      }
    }
  }
  bool configured = false;
  bool any = false;
  String failures;
  const size_t eventLimit = min<size_t>(
      kMaximumCalendarEvents, max<uint8_t>(1, config.calendarMaximumEvents));
  events.reserve(eventLimit);
  for (const CalendarItem& item : items) {
    if (!item.url.startsWith("https://") &&
        !item.url.startsWith("http://")) {
      continue;
    }
    configured = true;
    std::vector<CalendarEvent> feedEvents;
    String feedError;
    String name = item.name.isEmpty() ? String("Calendar") : item.name;
    truncateUtf8(name, 48);
    if (fetchCalendarUrl(item.url, name, item.iconId, config, feedEvents,
                         feedError)) {
      any = true;
      for (CalendarEvent& candidate : feedEvents) {
        truncateUtf8(candidate.title, kMaximumCalendarTitleBytes);
        bool duplicate = false;
        for (const CalendarEvent& existing : events) {
          if (existing.id == candidate.id) {
            duplicate = true;
            break;
          }
        }
        if (duplicate) continue;
        if (events.size() < eventLimit) {
          events.push_back(std::move(candidate));
          continue;
        }
        auto latest = std::max_element(
            events.begin(), events.end(),
            [](const CalendarEvent& left, const CalendarEvent& right) {
              return left.startsAt < right.startsAt;
            });
        if (latest != events.end() && candidate.startsAt < latest->startsAt) {
          *latest = std::move(candidate);
        }
      }
    } else if (!feedError.isEmpty()) {
      if (!failures.isEmpty()) failures += "; ";
      failures += name + ": " + feedError;
      if (failures.length() > 384) failures.remove(384);
    }
  }
  if (!configured) {
    error = "Calendar iCal URL is not configured";
    return false;
  }
  std::sort(events.begin(), events.end(),
            [](const CalendarEvent& left, const CalendarEvent& right) {
              return left.startsAt < right.startsAt;
            });
  if (events.size() > eventLimit) {
    events.resize(eventLimit);
  }
  error = failures;
  return any && !events.empty();
}

bool DataServices::fetchCalendarUrl(
    const String& url, const String& source, uint32_t iconId,
    const AppConfig& config,
    std::vector<CalendarEvent>& events, String& error) {
  WiFiClient plainClient;
  SafeTlsClient& secureClient = resetProviderSecureClient();
  WiFiClient* client = &plainClient;
  if (url.startsWith("https://")) {
    client = &secureClient;
  }
  // Bound a stalled read below the IDLE0 watchdog window. The loop below also
  // tracks aggregate idle time, so a half-open calendar socket cannot keep
  // the online worker alive indefinitely.
  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(2000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  if (!http.begin(*client, url)) {
    error = "Calendar iCal URL failed";
    return false;
  }
  const int status = http.GET();
  if (status < 200 || status >= 300) {
    error = "Calendar HTTP " + String(status);
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength > 262144) {
    error = "Calendar response is too large";
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  bool inEvent = false;
  bool inAlarm = false;
  CalendarEvent event;
  String uid;
  String recurrence;
  uint32_t alertLeadSeconds = 0;
  size_t total = 0;
  const uint32_t now = time(nullptr);
  uint32_t lastDataMs = millis();
  const size_t feedEventLimit = min<size_t>(
      kMaximumCalendarCandidatesPerFeed,
      max<size_t>(8, min<size_t>(kMaximumCalendarEvents,
                                 config.calendarMaximumEvents) *
                         2U));
  while ((http.connected() || stream->available() > 0) && total < 262144 &&
         (contentLength < 0 || total < static_cast<size_t>(contentLength)) &&
         events.size() < feedEventLimit) {
    if (stream->available() <= 0) {
      if (!http.connected() || millis() - lastDataMs > kHttpTimeoutMs) break;
      // Do not call readStringUntil() on an empty keep-alive socket: it waits
      // for the full stream timeout and used to add seconds to every calendar.
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    String line = stream->readStringUntil('\n');
    vTaskDelay(1);
    if (line.isEmpty()) {
      if (millis() - lastDataMs > kHttpTimeoutMs) break;
      continue;
    }
    lastDataMs = millis();
    total += line.length() + 1;
    line.trim();
    if (line == "BEGIN:VEVENT") {
      inEvent = true;
      inAlarm = false;
      event = CalendarEvent{};
      event.source = source;
      event.iconId = iconId;
      uid = "";
      recurrence = "";
      alertLeadSeconds = 0;
      continue;
    }
    if (!inEvent) {
      continue;
    }
    if (line == "BEGIN:VALARM") {
      inAlarm = true;
      continue;
    }
    if (line == "END:VALARM") {
      inAlarm = false;
      continue;
    }
    if (line == "END:VEVENT") {
      inEvent = false;
      if (event.title.isEmpty() || event.startsAt == 0) {
        continue;
      }
      if (event.endsAt == 0) {
        event.endsAt = event.startsAt + (event.allDay ? 86400UL : 3600UL);
      }
      const bool recurrenceValid =
          recurrence.isEmpty() || advanceRecurringEvent(event, recurrence, now);
      if (recurrenceValid && event.endsAt >= now &&
          event.startsAt <= now + 90UL * 86400UL) {
        event.id = fnv1a(uid + String(event.startsAt) + event.title);
        if (alertLeadSeconds != 0 && event.startsAt > alertLeadSeconds) {
          event.alertAt = event.startsAt - alertLeadSeconds;
        }
        events.push_back(event);
      }
      continue;
    }
    const int colon = line.indexOf(':');
    if (colon <= 0) {
      continue;
    }
    String key = line.substring(0, colon);
    String value = line.substring(colon + 1);
    const int semicolon = key.indexOf(';');
    if (semicolon >= 0) {
      key = key.substring(0, semicolon);
    }
    if (key == "SUMMARY") {
      if (value.length() > kMaximumCalendarTitleBytes * 3U) {
        value.remove(kMaximumCalendarTitleBytes * 3U);
      }
      event.title = decodeIcalText(value);
      truncateUtf8(event.title, kMaximumCalendarTitleBytes);
    } else if (key == "UID") {
      uid = value;
    } else if (key == "DTSTART") {
      event.startsAt = parseCompactDateTime(value, event.allDay);
    } else if (key == "DTEND") {
      bool endAllDay = false;
      event.endsAt = parseCompactDateTime(value, endAllDay);
    } else if (key == "RRULE") {
      recurrence = value;
    } else if (inAlarm && key == "TRIGGER") {
      alertLeadSeconds = parseIcalTriggerSeconds(value);
    }
  }
  http.end();
  for (CalendarEvent& value : events) {
    if (value.endsAt == 0) {
      value.endsAt = value.startsAt + (value.allDay ? 86400UL : 3600UL);
    }
  }
  if (events.empty()) {
    error = "Calendar has no upcoming events";
    return false;
  }
  error = "";
  return true;
}

bool DataServices::fetchTravel(
    const AppConfig& config, std::vector<TravelStopSnapshot>& stops,
    String& error) {
  const std::vector<TravelStopItem> configured =
      parseTravelStopItems(config.travelStopItems, kMaximumTravelStopItems);
  if (configured.empty()) {
    error = "Travel is not configured";
    return false;
  }

  String stopIds;
  stopIds.reserve(configured.size() * 8U);
  for (size_t index = 0; index < configured.size(); ++index) {
    bool alreadyAdded = false;
    for (size_t previous = 0; previous < index; ++previous) {
      if (configured[previous].stopId.equalsIgnoreCase(
              configured[index].stopId)) {
        alreadyAdded = true;
        break;
      }
    }
    if (alreadyAdded) continue;
    if (!stopIds.isEmpty()) stopIds += ',';
    stopIds += configured[index].stopId;
  }
  if (stopIds.isEmpty()) {
    error = "Travel is not configured";
    return false;
  }

  const uint32_t requestEpoch = static_cast<uint32_t>(time(nullptr));
  if (requestEpoch <= 1672531200) {
    error = "Travel clock is not synchronized";
    return false;
  }
  const String url =
      "https://transport.tallinn.ee/siri-stop-departures.php?stopid=" +
      stopIds + "&time=" + String(requestEpoch) + "000";

  SafeTlsClient& client = resetProviderSecureClient();
  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);
  http.useHTTP10(true);
  if (!http.begin(client, url)) {
    error = "Travel connection failed";
    return false;
  }
  http.setUserAgent(kUserAgent);
  http.addHeader("Accept", "text/plain,text/csv");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Connection", "close");
  const int status = http.GET();
  if (status < 200 || status >= 300) {
    error = status < 0
                ? String("Travel connection: ") +
                      HTTPClient::errorToString(status)
                : String("Travel HTTP ") + String(status);
    http.end();
    return false;
  }
  const int contentLength = http.getSize();
  if (contentLength >
      static_cast<int>(pikos::TallinnSiriParser::kMaximumBodyBytes)) {
    error = "Travel response is too large";
    http.end();
    return false;
  }

  stops.clear();
  stops.resize(configured.size());
  const uint32_t fetchedAt = static_cast<uint32_t>(time(nullptr));
  for (size_t index = 0; index < configured.size(); ++index) {
    TravelStopSnapshot& snapshot = stops[index];
    snapshot.label = configured[index].label;
    snapshot.stopId = configured[index].stopId;
    snapshot.line = configured[index].line;
    snapshot.iconId = configured[index].iconId;
    snapshot.fetchedAt = fetchedAt;
  }

  TravelParserContext context;
  context.configured = &configured;
  context.snapshots = &stops;
  context.fetchedAt = fetchedAt;
  pikos::TallinnSiriParser parser(collectTravelDeparture, &context);
  parser.setStopCallback(collectTravelStop);
  WiFiClient* stream = http.getStreamPtr();
  bool timedOut = false;
  const uint32_t streamStartedMs = millis();
  uint32_t lastDataMs = millis();
  while (http.connected() || stream->available() > 0) {
    if (millis() - streamStartedMs > kTravelResponseWallClockMs) {
      timedOut = true;
      break;
    }
    const int available = stream->available();
    if (available <= 0) {
      if (millis() - lastDataMs > kHttpTimeoutMs) {
        timedOut = true;
        break;
      }
      vTaskDelay(1);
      continue;
    }
    const int chunk = min(available, 512);
    for (int index = 0; index < chunk; ++index) {
      const int byte = stream->read();
      if (byte < 0) break;
      if (!parser.feed(static_cast<uint8_t>(byte))) break;
    }
    if (parser.error() != pikos::TallinnSiriParser::Error::None) break;
    lastDataMs = millis();
    vTaskDelay(1);
  }
  const bool completeLength =
      contentLength < 0 ||
      parser.bodyBytes() == static_cast<size_t>(contentLength);
  const bool parsed = !timedOut && completeLength && parser.finish();
  http.end();
  if (!parsed) {
    stops.clear();
    if (timedOut) {
      error = "Travel response timed out";
    } else if (!completeLength) {
      error = "Travel response was incomplete";
    } else if (parser.error() ==
                   pikos::TallinnSiriParser::Error::MissingStop ||
               strcmp(parser.errorMessage(), "missing stop block") == 0) {
      // Tallinn silently returns only the header for an internal platform
      // code such as `10801-1`. Its realtime endpoint requires the numeric
      // SIRI ID (`1234` for that example), which the browser picker supplies.
      error = "Unknown stop ID; use Tallinn stop search";
    } else {
      error = String("Travel parser: ") + parser.errorMessage();
    }
    return false;
  }
  if (context.returnedStopMask == 0) {
    stops.clear();
    error = "Unknown stop ID; use Tallinn stop search";
    return false;
  }

  bool missingStop = false;
  for (size_t index = 0; index < stops.size(); ++index) {
    TravelStopSnapshot& snapshot = stops[index];
    if ((context.returnedStopMask & static_cast<uint16_t>(1U << index)) == 0) {
      snapshot.error = "Unknown stop ID; use Tallinn stop search";
      missingStop = true;
      continue;
    }
    if (snapshot.departureCount != 0) continue;
    snapshot.error = snapshot.line.isEmpty()
                         ? "No upcoming departures"
                         : "No matching departures";
  }
  error = missingStop ? "Some stop IDs were not found" : String();
  return true;
}

bool DataServices::fetchReddit(const AppConfig& config,
                               std::vector<Headline>& posts,
                               String& error) {
  const std::vector<IconItem> configured =
      parseIconItems(config.redditItems, 16);
  std::vector<IconItem> sources;
  String combinedNames;
  for (const IconItem& item : configured) {
    const String safe = safeSubreddit(item.value);
    if (safe.isEmpty()) continue;
    if (!combinedNames.isEmpty()) combinedNames += '+';
    combinedNames += safe;
    sources.emplace_back(safe, item.iconId);
  }
  if (sources.empty()) {
    error = "Reddit subreddits are not configured";
    return false;
  }

  // Reddit now rate-limits consecutive account-free RSS calls: the first
  // subreddit succeeds and the rest receive HTTP 429. Reddit's supported
  // multi-subreddit path returns the same Atom entries in one request. The
  // parser retains each entry's source/category so custom icons and PikOS's
  // round-robin presentation still work.
  const String url = "https://www.reddit.com/r/" + combinedNames +
                     "/top/.rss?t=" + config.redditPeriod;
  const uint8_t combinedMaximum = static_cast<uint8_t>(
      min<size_t>(15, max<size_t>(config.redditPostCount,
                                  sources.size() * 3U)));
  std::vector<Headline> combined(combinedMaximum);
  for (Headline& post : combined) {
    post.source.reserve(24);
    post.sourceUrl.reserve(96);
    post.title.reserve(config.maximumMessageChars + 4U);
  }
  bool fetched = false;
  {
    ScopedTlsHeapLease tlsHeap;
    fetched = fetchRssUrl(url, "Reddit", combinedMaximum,
                          config.maximumMessageChars, combined, error,
                          &sources);
  }
  Serial.printf("[reddit] combined %u sources: %s (%u posts)%s%s\n",
                static_cast<unsigned>(sources.size()),
                fetched ? "ok" : "failed",
                static_cast<unsigned>(fetched ? combined.size() : 0),
                error.isEmpty() ? "" : " — ", error.c_str());
  if (!fetched) return false;

  std::vector<std::vector<Headline>> feeds(sources.size());
  uint16_t unknown = 0;
  for (Headline& post : combined) {
    const String postSource = safeSubreddit(post.source);
    bool assigned = false;
    for (size_t index = 0; index < sources.size(); ++index) {
      if (safeSubreddit(sources[index].value).equalsIgnoreCase(postSource)) {
        feeds[index].push_back(std::move(post));
        assigned = true;
        break;
      }
    }
    if (!assigned) ++unknown;
  }
  size_t maximumDepth = 0;
  for (size_t index = 0; index < sources.size(); ++index) {
    maximumDepth = max(maximumDepth, feeds[index].size());
    Serial.printf("[reddit] r/%s: %u posts\n", sources[index].value.c_str(),
                  static_cast<unsigned>(feeds[index].size()));
  }
  if (unknown != 0) {
    Serial.printf("[reddit] %u combined entries had no source tag\n",
                  static_cast<unsigned>(unknown));
  }

  // Match RSS: one post from every configured subreddit before showing a
  // second post from any of them. A short slideshow visit cannot be
  // monopolised by the first or largest community.
  posts.reserve(config.redditPostCount);
  for (size_t item = 0;
       item < maximumDepth && posts.size() < config.redditPostCount; ++item) {
    for (size_t feedIndex = 0; feedIndex < feeds.size(); ++feedIndex) {
      if (item < feeds[feedIndex].size()) {
        posts.push_back(std::move(feeds[feedIndex][item]));
      }
      if (posts.size() >= config.redditPostCount) break;
    }
  }
  if (posts.empty()) {
    error = "Reddit feed entries had no matching subreddit";
    return false;
  }
  error = "";
  return true;
}

bool DataServices::fetchEnergy(const AppConfig& config,
                               EnergyPriceReading& reading,
                               std::vector<EnergyPricePoint>& points) {
  const time_t now = time(nullptr);
  if (now < 1672531200) {
    reading.error = "Energy prices need a valid clock";
    return false;
  }

  tm localStart{};
  localtime_r(&now, &localStart);
  localStart.tm_hour = 0;
  localStart.tm_min = 0;
  localStart.tm_sec = 0;
  localStart.tm_isdst = -1;
  const time_t todayStart = mktime(&localStart);
  tm endLocal = localStart;
  endLocal.tm_mday += 2;
  endLocal.tm_isdst = -1;
  const time_t rangeEnd = mktime(&endLocal);

  auto isoUtc = [](time_t value) {
    tm utc{};
    gmtime_r(&value, &utc);
    char output[25];
    strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return String(output);
  };

  String country = config.energyCountry;
  country.toLowerCase();
  // The JSON route returns every Baltic/Finnish area. Since Nord Pool moved
  // to 15-minute prices that response became needlessly large for an ESP32.
  // The official CSV route can filter to one area, reducing TLS/heap pressure
  // by roughly fourfold.
  const String url =
      "https://dashboard.elering.ee/api/nps/price/csv?start=" +
      formEncode(isoUtc(todayStart)) + "&end=" +
      formEncode(isoUtc(rangeEnd)) + "&fields=" + country;

  String payload;
  int status = 0;
  for (uint8_t attempt = 0; attempt < 2; ++attempt) {
    SafeTlsClient& client = resetProviderSecureClient();
    HTTPClient http;
    http.setConnectTimeout(kHttpTimeoutMs);
    http.setTimeout(kHttpTimeoutMs);
    http.setReuse(false);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
      status = HTTPC_ERROR_CONNECTION_REFUSED;
    } else {
      http.setUserAgent(kUserAgent);
      http.addHeader("Accept", "text/csv");
      http.addHeader("Accept-Encoding", "identity");
      http.addHeader("Connection", "close");
      status = http.GET();
      if (status >= 200 && status < 300) payload = http.getString();
      http.end();
    }
    if (!payload.isEmpty()) break;
    if (attempt < 1) vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (payload.isEmpty()) {
    reading.error = "Elering connection: " +
                    (status < 0 ? HTTPClient::errorToString(status)
                                : String("HTTP ") + String(status));
    return false;
  }

  points.clear();
  points.reserve(kMaximumEnergyPricePoints);
  const float vatFactor =
      config.energyIncludeVat ? 1.0f + config.energyVatPercent / 100.0f
                              : 1.0f;
  size_t cursor = 0;
  while (cursor < payload.length()) {
    int lineEnd = payload.indexOf('\n', cursor);
    if (lineEnd < 0) lineEnd = payload.length();
    String line = payload.substring(cursor, lineEnd);
    cursor = static_cast<size_t>(lineEnd) + 1;
    line.trim();
    const int firstSeparator = line.indexOf(';');
    const int secondSeparator =
        firstSeparator < 0 ? -1 : line.indexOf(';', firstSeparator + 1);
    if (firstSeparator < 0 || secondSeparator < 0) continue;
    String timestampText = line.substring(0, firstSeparator);
    String priceText = line.substring(secondSeparator + 1);
    timestampText.replace("\"", "");
    priceText.replace("\"", "");
    priceText.replace(",", ".");
    const uint32_t timestamp = strtoul(timestampText.c_str(), nullptr, 10);
    const float eurMwh = priceText.toFloat();
    if (timestamp == 0) continue;  // skips the localized header row
    // Elering prices are EUR/MWh; divide by ten for euro-cents/kWh.
    const float centsKwh = eurMwh * vatFactor / 10.0f;
    if (isfinite(centsKwh) && points.size() < kMaximumEnergyPricePoints) {
      points.push_back({timestamp, centsKwh});
    }
  }
  if (points.empty()) {
    reading.error = "Elering returned no prices for today";
    return false;
  }
  std::sort(points.begin(), points.end(),
            [](const EnergyPricePoint& a, const EnergyPricePoint& b) {
              return a.timestamp < b.timestamp;
            });
  points.erase(std::unique(points.begin(), points.end(),
                           [](const EnergyPricePoint& left,
                              const EnergyPricePoint& right) {
                             return left.timestamp == right.timestamp;
                           }),
               points.end());

  EnergyPriceReading derived;
  derived.error = "";
  if (!deriveEnergyReading(derived, points, config.energyChartRange, now,
                           derived)) {
    reading = derived;
    if (reading.error.isEmpty()) {
      reading.error = "Elering returned no prices for today";
    }
    return false;
  }
  derived.error = "";
  reading = derived;
  return true;
}

OnlineStatus DataServices::statusSnapshot() const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  OnlineStatus status;
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  status.headlineCount = data_.headlines.size();
  status.marketCount = data_.marketQuotes.size();
  for (const CalendarEvent& event : data_.calendarEvents) {
    if (liveCalendarEvent(event, now)) ++status.calendarCount;
  }
  status.travelCount = data_.travelStops.size();
  status.redditCount = data_.redditPosts.size();
  status.activityConnected =
      data_.activity.valid && !data_.activity.reconnectRequired;
  status.activity = data_.activity;
  status.marketChartPoints = data_.marketQuotes.empty()
                                 ? 0
                                 : data_.marketQuotes.front().sparklineCount;
  status.netatmo = data_.netatmo;
  deriveEnergyReading(data_.energy, data_.energyPoints,
                      data_.energyChartRange, now, status.energy);
  status.rssUpdatedAt = data_.rssUpdatedAt;
  status.netatmoUpdatedAt = data_.netatmoUpdatedAt;
  status.marketUpdatedAt = data_.marketUpdatedAt;
  status.energyUpdatedAt = data_.energyUpdatedAt;
  status.calendarUpdatedAt = data_.calendarUpdatedAt;
  status.travelUpdatedAt = data_.travelUpdatedAt;
  status.redditUpdatedAt = data_.redditUpdatedAt;
  status.activityUpdatedAt = data_.activityUpdatedAt;
  status.rssError = data_.rssError;
  status.marketError = data_.marketError;
  status.calendarError = data_.calendarError;
  status.travelError = data_.travelError;
  status.redditError = data_.redditError;
  status.activityError = data_.activityError;
  status.travelStale =
      data_.travelUpdatedAt == 0 || now <= 1672531200 ||
      now < data_.travelUpdatedAt ||
      now - data_.travelUpdatedAt > kTravelPredictionStaleSeconds;
  xSemaphoreGive(mutex_);
  return status;
}

NetatmoReading DataServices::netatmoSnapshot() const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const NetatmoReading reading = data_.netatmo;
  xSemaphoreGive(mutex_);
  return reading;
}

EnergyPriceReading DataServices::energySnapshot() const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  EnergyPriceReading reading;
  deriveEnergyReading(data_.energy, data_.energyPoints,
                      data_.energyChartRange, time(nullptr), reading);
  xSemaphoreGive(mutex_);
  return reading;
}

ActivitySnapshot DataServices::activitySnapshot() const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const ActivitySnapshot activity = data_.activity;
  xSemaphoreGive(mutex_);
  return activity;
}

bool DataServices::headlineAt(size_t index, Headline& value, size_t& count,
                              String& error) const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  count = data_.headlines.size();
  error = data_.rssError;
  const bool available = count != 0;
  if (available) value = data_.headlines[index % count];
  xSemaphoreGive(mutex_);
  return available;
}

bool DataServices::headlineById(uint32_t id, Headline& value) const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  bool available = !data_.headlines.empty();
  if (available) {
    value = data_.headlines.front();
    if (id != 0) {
      for (const Headline& candidate : data_.headlines) {
        if (candidate.id == id) {
          value = candidate;
          break;
        }
      }
    }
  }
  xSemaphoreGive(mutex_);
  return available;
}

bool DataServices::marketAt(size_t index, MarketQuote& value, size_t& count,
                            String& error) const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  count = data_.marketQuotes.size();
  error = data_.marketError;
  const bool available = count != 0;
  if (available) value = data_.marketQuotes[index % count];
  xSemaphoreGive(mutex_);
  return available;
}

bool DataServices::calendarAt(size_t index, CalendarEvent& value,
                              size_t& count, String& error) const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  count = 0;
  error = data_.calendarError;
  for (const CalendarEvent& event : data_.calendarEvents) {
    if (liveCalendarEvent(event, now)) ++count;
  }
  const bool available = count != 0;
  if (available) {
    const size_t selected = index % count;
    size_t liveIndex = 0;
    for (const CalendarEvent& event : data_.calendarEvents) {
      if (!liveCalendarEvent(event, now)) continue;
      if (liveIndex++ == selected) {
        value = event;
        break;
      }
    }
  } else if (error.isEmpty()) {
    error = "Calendar has no upcoming events";
  }
  xSemaphoreGive(mutex_);
  return available;
}

bool DataServices::travelAt(size_t index, TravelStopSnapshot& value,
                            size_t& count, String& error,
                            bool& stale) const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const uint32_t now = static_cast<uint32_t>(time(nullptr));
  count = data_.travelStops.size();
  error = data_.travelError;
  const bool available = count != 0;
  stale = true;
  if (available) {
    value = data_.travelStops[index % count];
    stale = value.fetchedAt == 0 || now <= 1672531200 ||
            now < value.fetchedAt ||
            now - value.fetchedAt > kTravelPredictionStaleSeconds;
    const uint8_t cachedCount = value.departureCount;
    value.departureCount = 0;
    for (uint8_t departureIndex = 0;
         departureIndex < cachedCount &&
         departureIndex < kMaximumTravelDepartures;
         ++departureIndex) {
      const TravelDeparture& departure = value.departures[departureIndex];
      const uint32_t shownAt =
          stale && departure.scheduledAt != 0 ? departure.scheduledAt
                                              : departure.expectedAt;
      // Keep a just-departed service visible for half a minute so small clock
      // skew cannot turn `1m` directly into a misleading provider error.
      if (shownAt == 0 ||
          (now > shownAt && now - shownAt > 30U)) {
        continue;
      }
      value.departures[value.departureCount++] = departure;
    }
    if (value.departureCount == 0 && value.error.isEmpty()) {
      value.error = "No upcoming departures";
    }
  } else if (error.isEmpty()) {
    error = "Travel has no saved departures";
  }
  xSemaphoreGive(mutex_);
  return available;
}

bool DataServices::redditAt(size_t index, Headline& value, size_t& count,
                            String& error) const {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  count = data_.redditPosts.size();
  error = data_.redditError;
  const bool available = count != 0;
  if (available) value = data_.redditPosts[index % count];
  xSemaphoreGive(mutex_);
  return available;
}

uint16_t DataServices::syncProgressPermille() const {
  if (!refreshInProgress()) return 1000;
  if (!syncActive_) {
    // A freshly requested/offline cycle has not learned its enabled mask yet.
    // Conversely, the worker may still be unwinding for a few instructions
    // after marking the final active cycle complete.
    return forceRefreshMask_ != 0 ? 0 : (taskRunning_ ? 999 : 1000);
  }

  const uint8_t mask = syncStartedProviderMask_;
  const uint8_t totalProviders = bitCount(mask);
  if (totalProviders == 0) return 0;

  uint32_t totalWeightMs =
      static_cast<uint32_t>(totalProviders - 1) * kProviderServiceGapMs;
  uint32_t completedWeightMs = 0;
  uint8_t ordinal = 0;
  int8_t activeOrdinal = -1;
  const OnlineProvider active = activeProvider_;
  for (OnlineProvider provider : kProviderRunOrder) {
    if ((mask & providerBit(provider)) == 0) continue;
    const int8_t index = providerIndex(provider);
    const uint32_t estimate =
        index >= 0 && syncProviderEstimateMs_[index] != 0
            ? syncProviderEstimateMs_[index]
            : defaultProviderDurationMs(provider);
    totalWeightMs += estimate;
    if (ordinal < syncCompleted_) completedWeightMs += estimate;
    if (provider == active) activeOrdinal = ordinal;
    ++ordinal;
  }

  // Each completed provider except the most recent has necessarily also
  // completed its inter-provider service window. If another provider is
  // already active, the most recent window has completed as well.
  uint8_t completeGaps = syncCompleted_ == 0 ? 0 : syncCompleted_ - 1;
  if (active != OnlineProvider::None &&
      activeOrdinal >= static_cast<int8_t>(syncCompleted_)) {
    completeGaps = syncCompleted_;
  }
  completeGaps = min<uint8_t>(completeGaps, totalProviders - 1);
  completedWeightMs +=
      static_cast<uint32_t>(completeGaps) * kProviderServiceGapMs;

  if (active != OnlineProvider::None &&
      activeOrdinal >= static_cast<int8_t>(syncCompleted_)) {
    const int8_t index = providerIndex(active);
    const uint32_t estimate =
        index >= 0 && syncProviderEstimateMs_[index] != 0
            ? syncProviderEstimateMs_[index]
            : defaultProviderDurationMs(active);
    const uint32_t elapsed =
        providerStartedAtMs_ == 0 ? 0 : millis() - providerStartedAtMs_;
    // Reserve the final five percent for the actual completion signal. A
    // slow endpoint therefore approaches its boundary without falsely
    // claiming that the next provider has started.
    completedWeightMs += min<uint32_t>(elapsed, estimate * 95UL / 100UL);
  } else if (syncActive_ && syncCompleted_ != 0 &&
             syncCompleted_ < totalProviders && nextJobEarliestMs_ != 0) {
    const uint32_t nowMs = millis();
    const uint32_t gapStartedMs = nextJobEarliestMs_ - kProviderServiceGapMs;
    const uint32_t gapElapsed = nowMs - gapStartedMs;
    completedWeightMs += min<uint32_t>(gapElapsed, kProviderServiceGapMs);
  }

  if (totalWeightMs == 0) return 0;
  return static_cast<uint16_t>(min<uint32_t>(
      999, (completedWeightMs * 1000ULL) / totalWeightMs));
}

SyncTelemetry DataServices::syncTelemetry() const {
  SyncTelemetry telemetry;
  const uint32_t nowMs = millis();
  telemetry.activeProvider = activeProvider_;
  telemetry.cyclesStarted = syncCyclesStarted_;
  telemetry.cyclesCompleted = syncCyclesCompleted_;
  telemetry.cyclesAborted = syncCyclesAborted_;
  telemetry.providerJobs = providerJobs_;
  telemetry.providerFailures = providerFailures_;
  telemetry.currentSyncElapsedMs =
      refreshInProgress() && syncStartedAtMs_ != 0
          ? nowMs - syncStartedAtMs_
          : 0;
  telemetry.currentProviderElapsedMs =
      taskRunning_ && providerStartedAtMs_ != 0
          ? nowMs - providerStartedAtMs_
          : 0;
  telemetry.lastSyncDurationMs = lastSyncDurationMs_;
  telemetry.averageSyncDurationMs = averageSyncDurationMs_;
  telemetry.lastProviderDurationMs = lastProviderDurationMs_;
  telemetry.maximumProviderDurationMs = maximumProviderDurationMs_;
  telemetry.quarantinedProviderMask = quarantinedProviderMask_;
  return telemetry;
}

DataCacheTelemetry DataServices::cacheTelemetry() const {
  DataCacheTelemetry telemetry;
  xSemaphoreTake(mutex_, portMAX_DELAY);
  telemetry.headlines = static_cast<uint16_t>(
      min<size_t>(UINT16_MAX, data_.headlines.size()));
  telemetry.marketQuotes = static_cast<uint16_t>(
      min<size_t>(UINT16_MAX, data_.marketQuotes.size()));
  telemetry.energyPoints = static_cast<uint16_t>(
      min<size_t>(UINT16_MAX, data_.energyPoints.size()));
  telemetry.calendarEvents = static_cast<uint16_t>(
      min<size_t>(UINT16_MAX, data_.calendarEvents.size()));
  telemetry.travelStops = static_cast<uint16_t>(
      min<size_t>(UINT16_MAX, data_.travelStops.size()));
  telemetry.redditPosts = static_cast<uint16_t>(
      min<size_t>(UINT16_MAX, data_.redditPosts.size()));
  telemetry.activityRecords = data_.activity.valid ? 1 : 0;

  uint64_t rssBytes =
      data_.headlines.capacity() * sizeof(Headline) +
      data_.rssError.length();
  uint64_t redditBytes =
      data_.redditPosts.capacity() * sizeof(Headline) +
      data_.redditError.length();
  auto addHeadlineStrings = [](uint64_t& bytes,
                               const std::vector<Headline>& values) {
    for (const Headline& value : values) {
      bytes += value.source.length() + value.sourceUrl.length() +
               value.title.length();
    }
  };
  addHeadlineStrings(rssBytes, data_.headlines);
  addHeadlineStrings(redditBytes, data_.redditPosts);

  uint64_t marketBytes =
      data_.marketQuotes.capacity() * sizeof(MarketQuote) +
      data_.marketError.length();
  for (const MarketQuote& quote : data_.marketQuotes) {
    marketBytes += quote.symbol.length() + quote.name.length() +
                   quote.currency.length() + quote.error.length();
  }

  uint64_t calendarBytes =
      data_.calendarEvents.capacity() * sizeof(CalendarEvent) +
      data_.calendarError.length();
  for (const CalendarEvent& event : data_.calendarEvents) {
    calendarBytes += event.source.length() + event.title.length();
  }

  uint64_t travelBytes =
      data_.travelStops.capacity() * sizeof(TravelStopSnapshot) +
      data_.travelError.length();
  for (const TravelStopSnapshot& stop : data_.travelStops) {
    travelBytes += stop.label.length() + stop.stopId.length() +
                   stop.line.length() + stop.error.length();
    for (uint8_t index = 0;
         index < stop.departureCount && index < kMaximumTravelDepartures;
         ++index) {
      travelBytes += stop.departures[index].route.length() +
                     stop.departures[index].destination.length();
    }
  }

  const uint64_t climateBytes =
      sizeof(NetatmoReading) + data_.netatmo.moduleName.length() +
      data_.netatmo.error.length();
  const uint64_t energyBytes =
      sizeof(EnergyPriceReading) +
      data_.energyPoints.capacity() * sizeof(EnergyPricePoint) +
      data_.energy.error.length() + data_.energyChartRange.length();
  const uint64_t activityBytes =
      sizeof(ActivitySnapshot) + data_.activity.latestName.length() +
      data_.activity.error.length() + data_.activityError.length();
  telemetry.rssBytes =
      static_cast<uint32_t>(min<uint64_t>(UINT32_MAX, rssBytes));
  telemetry.climateBytes =
      static_cast<uint32_t>(min<uint64_t>(UINT32_MAX, climateBytes));
  telemetry.marketBytes =
      static_cast<uint32_t>(min<uint64_t>(UINT32_MAX, marketBytes));
  telemetry.energyBytes =
      static_cast<uint32_t>(min<uint64_t>(UINT32_MAX, energyBytes));
  telemetry.calendarBytes =
      static_cast<uint32_t>(min<uint64_t>(UINT32_MAX, calendarBytes));
  telemetry.travelBytes =
      static_cast<uint32_t>(min<uint64_t>(UINT32_MAX, travelBytes));
  telemetry.redditBytes =
      static_cast<uint32_t>(min<uint64_t>(UINT32_MAX, redditBytes));
  telemetry.activityBytes =
      static_cast<uint32_t>(min<uint64_t>(UINT32_MAX, activityBytes));
  const uint64_t bytes = rssBytes + climateBytes + marketBytes + energyBytes +
                         calendarBytes + travelBytes + redditBytes +
                         activityBytes;
  telemetry.estimatedBytes = static_cast<uint32_t>(
      min<uint64_t>(UINT32_MAX, bytes));
  xSemaphoreGive(mutex_);
  return telemetry;
}

bool DataServices::consumeNewHeadlineEvent(uint32_t& headlineId) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool result = newHeadlineEvent_;
  newHeadlineEvent_ = false;
  headlineId = pendingHeadlineId_;
  pendingHeadlineId_ = 0;
  xSemaphoreGive(mutex_);
  return result;
}

bool DataServices::consumeRainStartEvent(NetatmoReading& reading) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool result = rainStartEvent_;
  if (result) {
    reading = pendingRainReading_;
    rainStartEvent_ = false;
  }
  xSemaphoreGive(mutex_);
  return result;
}

bool DataServices::consumeEnergyBandEvent() {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool result = energyBandEvent_;
  energyBandEvent_ = false;
  xSemaphoreGive(mutex_);
  return result;
}

bool DataServices::consumeCalendarAlertEvent(CalendarEvent& event) {
  xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool result = calendarAlertEvent_;
  if (result) {
    event = pendingCalendarAlert_;
    calendarAlertEvent_ = false;
  }
  xSemaphoreGive(mutex_);
  return result;
}

bool DataServices::requestRefresh() {
  if (taskRunning_ || syncActive_ || forceRefreshMask_ != 0) return false;
  // Disabled providers are removed from the mask on the next tick.
  syncRequestedAtMs_ = millis();
  offlineSinceMs_ = 0;
  forceRefreshMask_ = 0xff;
  return true;
}

void DataServices::quarantineProvider(OnlineProvider provider,
                                      const char* reason) {
  const uint8_t bit = providerBit(provider);
  if (bit == 0) return;
  const bool alreadyQuarantined = (quarantinedProviderMask_ & bit) != 0;
  quarantinedProviderMask_ |= bit;
  forceRefreshMask_ &= static_cast<uint8_t>(~bit);

  const String message = reason == nullptr || reason[0] == '\0'
                             ? String("Provider paused for this boot")
                             : String(reason);
  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
  switch (provider) {
    case OnlineProvider::Rss:
      data_.rssError = message;
      break;
    case OnlineProvider::Netatmo:
      data_.netatmo.error = message;
      break;
    case OnlineProvider::Market:
      data_.marketError = message;
      break;
    case OnlineProvider::Energy:
      data_.energy.error = message;
      break;
    case OnlineProvider::Calendar:
      data_.calendarError = message;
      break;
    case OnlineProvider::Travel:
      data_.travelError = message;
      break;
    case OnlineProvider::Reddit:
      data_.redditError = message;
      break;
    case OnlineProvider::Activity:
      data_.activityError = message;
      data_.activity.error = message;
      break;
    case OnlineProvider::None:
      break;
  }
  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
  if (!alreadyQuarantined) {
    Serial.printf("[data] Quarantined %s for this boot: %s\n",
                  providerLabel(provider), message.c_str());
  }
}

}  // namespace pixelos
