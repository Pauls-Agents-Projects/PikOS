#include "Config.h"

#include <SPIFFS.h>
#include <WiFi.h>
#include <esp_system.h>

#include <algorithm>
#include <memory>
#include <type_traits>

#include "TlsHeapGuard.h"

namespace pixelos {
namespace {

constexpr char kNamespace[] = "pixelos";
constexpr char kConfigKey[] = "config";
constexpr char kConfigBackupKey[] = "config_bak";
constexpr char kConfigPath[] = "/pikos-config.json";
constexpr char kConfigBackupPath[] = "/pikos-config.bak";
constexpr char kConfigTemporaryPath[] = "/pikos-config.tmp";
constexpr char kWifiPasswordKey[] = "wifi_pass";
constexpr char kNetatmoClientIdKey[] = "net_client";
constexpr char kNetatmoClientSecretKey[] = "net_secret";
constexpr char kNetatmoAccessTokenKey[] = "net_access";
constexpr char kNetatmoRefreshTokenKey[] = "net_refresh";
constexpr char kGarminCredentialsKey[] = "garmin_di";
constexpr char kMarketApiKeyKey[] = "market_key";
constexpr char kRemoteApiKeyKey[] = "remote_key";
constexpr char kCalendarIcalUrlKey[] = "cal_ical";
constexpr char kCalendarIcalUrl2Key[] = "cal_ical2";
constexpr char kCalendarIcalUrl3Key[] = "cal_ical3";
constexpr char kCalendarIcalUrl4Key[] = "cal_ical4";
constexpr uint8_t kMaximumCalendarItems = 16;
constexpr uint32_t kGarminCredentialMagic = 0x44474b50UL;  // "PKGD"
constexpr uint8_t kGarminCredentialSchema = 1;
constexpr size_t kMaximumGarminClientIdBytes = 128;
constexpr size_t kMaximumGarminRefreshTokenBytes = 3072;

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t value = 0xffffffffUL;
  for (size_t index = 0; index < length; ++index) {
    value ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      value = (value >> 1) ^
              (0xedb88320UL & static_cast<uint32_t>(-(value & 1U)));
    }
  }
  return value ^ 0xffffffffUL;
}

void appendLe16(std::vector<uint8_t>& output, uint16_t value) {
  output.push_back(static_cast<uint8_t>(value & 0xff));
  output.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
}

void appendLe32(std::vector<uint8_t>& output, uint32_t value) {
  for (uint8_t shift = 0; shift < 32; shift += 8) {
    output.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
  }
}

uint16_t readLe16(const uint8_t* value) {
  return static_cast<uint16_t>(value[0]) |
         static_cast<uint16_t>(value[1]) << 8;
}

uint32_t readLe32(const uint8_t* value) {
  return static_cast<uint32_t>(value[0]) |
         static_cast<uint32_t>(value[1]) << 8 |
         static_cast<uint32_t>(value[2]) << 16 |
         static_cast<uint32_t>(value[3]) << 24;
}

bool validGarminDomain(const String& value) {
  return value == "garmin.com" || value == "garmin.cn";
}

bool encodeGarminCredentials(const String& domain, const String& clientId,
                             const String& refreshToken,
                             std::vector<uint8_t>& output) {
  if (!validGarminDomain(domain) || clientId.isEmpty() ||
      refreshToken.isEmpty() ||
      clientId.length() > kMaximumGarminClientIdBytes ||
      refreshToken.length() > kMaximumGarminRefreshTokenBytes) {
    return false;
  }
  output.clear();
  output.reserve(12 + clientId.length() + refreshToken.length());
  appendLe32(output, kGarminCredentialMagic);
  output.push_back(kGarminCredentialSchema);
  output.push_back(domain == "garmin.cn" ? 1 : 0);
  appendLe16(output, static_cast<uint16_t>(clientId.length()));
  appendLe16(output, static_cast<uint16_t>(refreshToken.length()));
  output.insert(output.end(), clientId.c_str(),
                clientId.c_str() + clientId.length());
  output.insert(output.end(), refreshToken.c_str(),
                refreshToken.c_str() + refreshToken.length());
  appendLe32(output, crc32(output.data(), output.size()));
  return true;
}

bool decodeGarminCredentials(const uint8_t* data, size_t length,
                             String& domain, String& clientId,
                             String& refreshToken) {
  constexpr size_t kHeaderBytes = 10;
  constexpr size_t kChecksumBytes = 4;
  if (data == nullptr || length < kHeaderBytes + kChecksumBytes ||
      readLe32(data) != kGarminCredentialMagic ||
      data[4] != kGarminCredentialSchema || data[5] > 1) {
    return false;
  }
  const size_t clientLength = readLe16(data + 6);
  const size_t refreshLength = readLe16(data + 8);
  if (clientLength == 0 || refreshLength == 0 ||
      clientLength > kMaximumGarminClientIdBytes ||
      refreshLength > kMaximumGarminRefreshTokenBytes ||
      kHeaderBytes + clientLength + refreshLength + kChecksumBytes !=
          length ||
      crc32(data, length - kChecksumBytes) !=
          readLe32(data + length - kChecksumBytes)) {
    return false;
  }
  domain = data[5] == 1 ? "garmin.cn" : "garmin.com";
  clientId = String(reinterpret_cast<const char*>(data + kHeaderBytes),
                    clientLength);
  refreshToken = String(
      reinterpret_cast<const char*>(data + kHeaderBytes + clientLength),
      refreshLength);
  return true;
}

bool putGarminCredentials(Preferences& preferences, const String& domain,
                          const String& clientId,
                          const String& refreshToken) {
  if (clientId.isEmpty() && refreshToken.isEmpty()) {
    return !preferences.isKey(kGarminCredentialsKey) ||
           preferences.remove(kGarminCredentialsKey);
  }
  std::vector<uint8_t> encoded;
  if (!encodeGarminCredentials(domain, clientId, refreshToken, encoded)) {
    return false;
  }
  if (preferences.putBytes(kGarminCredentialsKey, encoded.data(),
                           encoded.size()) != encoded.size()) {
    return false;
  }
  // A rotated refresh token is accepted only after an immediate readback of
  // the complete checksummed blob. This keeps a reset during NVS compaction
  // from publishing a credential that cannot survive the next boot.
  std::vector<uint8_t> verified(encoded.size());
  if (preferences.getBytes(kGarminCredentialsKey, verified.data(),
                           verified.size()) != verified.size()) {
    return false;
  }
  String storedDomain;
  String storedClientId;
  String storedRefreshToken;
  return decodeGarminCredentials(verified.data(), verified.size(),
                                 storedDomain, storedClientId,
                                 storedRefreshToken) &&
         storedDomain == domain && storedClientId == clientId &&
         storedRefreshToken == refreshToken;
}

template <typename T>
void readIfPresent(JsonObjectConst object, const char* key, T& target) {
  if (!object[key].isNull()) {
    target = object[key].as<T>();
  }
}

void readSecretIfPresent(JsonObjectConst object, const char* key,
                         String& target) {
  if (!object[key].isNull()) {
    const String value = object[key].as<String>();
    if (!value.isEmpty()) {
      target = value;
    }
  }
}

String clampAnimation(const String& value) {
  if (value == "ticker" || value == "flip" || value == "static" ||
      value == "slide" || value == "rainbow" || value == "rain" ||
      value == "magic") {
    return value;
  }
  return "static";
}

String clampTone(const String& value, const char* fallback) {
  if (value == "off" || value == "click" || value == "double" ||
      value == "news" || value == "rise" || value == "soft" ||
      value == "alert" || value == "gran_vals" || value == "hourly" ||
      value == "custom1" || value == "custom2" || value == "custom3" ||
      value == "custom4" || value == "custom5" || value == "custom6") {
    return value;
  }
  return fallback;
}

String clampButtonAction(String value, const char* fallback) {
  value.trim();
  value.toLowerCase();
  const String allowed =
      ",previous,next,auto,refresh,device_info,counter_toggle,"
      "counter_reset,none,";
  return allowed.indexOf("," + value + ",") >= 0 ? value
                                                   : String(fallback);
}

String normalizeDisabledBuiltinTones(const String& encoded) {
  static constexpr const char* kBuiltins[] = {
      "click", "double", "news", "rise", "soft",
      "alert", "gran_vals", "hourly"};
  String normalized;
  size_t start = 0;
  while (start <= encoded.length()) {
    int end = encoded.indexOf(',', start);
    if (end < 0) end = encoded.length();
    String candidate = encoded.substring(start, end);
    candidate.trim();
    candidate.toLowerCase();
    bool valid = false;
    for (const char* builtin : kBuiltins) {
      if (candidate == builtin) {
        valid = true;
        break;
      }
    }
    if (valid && ("," + normalized + ",").indexOf("," + candidate + ",") < 0) {
      if (!normalized.isEmpty()) normalized += ',';
      normalized += candidate;
    }
    if (end >= static_cast<int>(encoded.length())) break;
    start = static_cast<size_t>(end + 1);
  }
  return normalized;
}

String legacyMappedIcon(const String& mappings, const String& wanted) {
  size_t start = 0;
  while (start <= mappings.length()) {
    int end = mappings.indexOf(',', start);
    if (end < 0) {
      end = mappings.length();
    }
    const String item = mappings.substring(start, end);
    const int equals = item.indexOf('=');
    if (equals > 0) {
      String key = item.substring(0, equals);
      key.trim();
      if (key.equalsIgnoreCase(wanted)) {
        String id = item.substring(equals + 1);
        id.trim();
        return id;
      }
    }
    if (static_cast<size_t>(end) >= mappings.length()) {
      break;
    }
    start = static_cast<size_t>(end) + 1;
  }
  return "0";
}

void appendIconItem(String& output, const String& value, uint32_t iconId) {
  if (value.isEmpty()) {
    return;
  }
  if (!output.isEmpty()) {
    output += '\n';
  }
  output += value;
  output += '\t';
  output += String(iconId);
}

void appendCalendarItem(String& output, const String& name,
                        const String& url, uint32_t iconId) {
  if (url.isEmpty()) {
    return;
  }
  if (!output.isEmpty()) {
    output += '\n';
  }
  output += name;
  output += '\t';
  output += url;
  output += '\t';
  output += String(iconId);
}

void appendTravelStopItem(String& output, const String& label,
                          const String& stopId, const String& line,
                          uint32_t iconId) {
  if (stopId.isEmpty()) return;
  if (!output.isEmpty()) output += '\n';
  output += label;
  output += '\t';
  output += stopId;
  output += '\t';
  output += line;
  output += '\t';
  output += String(iconId);
}

String calendarSecretKey(uint8_t index) {
  char key[12];
  snprintf(key, sizeof(key), "cal_dyn%02u", index);
  return String(key);
}

String redactedCalendarItems(const String& encoded) {
  String output;
  uint8_t index = 0;
  for (const CalendarItem& item :
       parseCalendarItems(encoded, kMaximumCalendarItems)) {
    appendCalendarItem(output, item.name, "@" + String(index++),
                       item.iconId);
  }
  return output;
}

String resolveCalendarItems(const String& submitted,
                            const String& existing) {
  const std::vector<CalendarItem> previous =
      parseCalendarItems(existing, kMaximumCalendarItems);
  String output;
  for (const CalendarItem& item :
       parseCalendarItems(submitted, kMaximumCalendarItems)) {
    String url = item.url;
    if (url.startsWith("@")) {
      const long oldIndex = strtol(url.c_str() + 1, nullptr, 10);
      url = oldIndex >= 0 && oldIndex < static_cast<long>(previous.size())
                ? previous[oldIndex].url
                : url;
    }
    appendCalendarItem(output, item.name, url, item.iconId);
  }
  return output;
}

bool putSecret(Preferences& preferences, const char* key,
               const String& value) {
  if (value.isEmpty()) {
    return !preferences.isKey(key) || preferences.remove(key);
  }
  if (preferences.isKey(key) &&
      preferences.getString(key, "") == value) {
    return true;
  }
  return preferences.putString(key, value) == value.length();
}

void loadOrMigrateSecret(Preferences& preferences, const char* key,
                         String& value) {
  const String stored =
      preferences.isKey(key) ? preferences.getString(key, "") : String();
  if (!stored.isEmpty()) {
    value = stored;
  } else if (!value.isEmpty()) {
    preferences.putString(key, value);
  }
}

String readConfigFile(const char* path) {
  File file = SPIFFS.open(path, FILE_READ);
  if (!file) {
    return "";
  }
  const String content = file.readString();
  file.close();
  return content;
}

bool writeConfigFile(const char* json, size_t jsonLength) {
  if (json == nullptr || jsonLength < 1000) {
    return false;
  }
  // A cached icon collection can leave room for the active settings file but
  // not active + backup + temporary simultaneously. Remove the expendable
  // old backup before allocating the new temporary file.
  if (SPIFFS.exists(kConfigTemporaryPath)) {
    SPIFFS.remove(kConfigTemporaryPath);
  }
  if (SPIFFS.exists(kConfigBackupPath)) {
    SPIFFS.remove(kConfigBackupPath);
  }
  File temporary = SPIFFS.open(kConfigTemporaryPath, FILE_WRITE);
  if (!temporary) {
    // Last-resort low-space path. It is less atomic, but keeps settings
    // writable instead of trapping the portal until its page is reloaded.
    File direct = SPIFFS.open(kConfigPath, FILE_WRITE);
    if (!direct) {
      return false;
    }
    const size_t directWritten = direct.write(
        reinterpret_cast<const uint8_t*>(json), jsonLength);
    direct.flush();
    direct.close();
    return directWritten == jsonLength;
  }
  const size_t written = temporary.write(
      reinterpret_cast<const uint8_t*>(json), jsonLength);
  temporary.flush();
  temporary.close();
  if (written != jsonLength) {
    SPIFFS.remove(kConfigTemporaryPath);
    return false;
  }

  if (SPIFFS.exists(kConfigPath) &&
      !SPIFFS.rename(kConfigPath, kConfigBackupPath)) {
    SPIFFS.remove(kConfigTemporaryPath);
    return false;
  }
  if (!SPIFFS.rename(kConfigTemporaryPath, kConfigPath)) {
    if (SPIFFS.exists(kConfigBackupPath)) {
      SPIFFS.rename(kConfigBackupPath, kConfigPath);
    }
    return false;
  }
  return true;
}

// ArduinoJson's in-memory representation of the roughly 150 flat settings
// fields is much larger than the JSON text itself.  Building the flat object
// directly keeps normal portal reads and saves from needing a contiguous
// 32 KiB heap allocation after the icon and network caches are active.
class JsonStringObject {
 public:
  class Value {
   public:
    Value(JsonStringObject& owner, const char* key)
        : owner_(owner), key_(key) {}

    Value& operator=(const String& value) {
      owner_.appendString(key_, value);
      return *this;
    }
    Value& operator=(const char* value) {
      owner_.appendString(key_, value == nullptr ? String() : String(value));
      return *this;
    }
    Value& operator=(bool value) {
      owner_.appendLiteral(key_, value ? "true" : "false");
      return *this;
    }
    Value& operator=(float value) {
      owner_.appendFloat(key_, value);
      return *this;
    }
    Value& operator=(double value) {
      owner_.appendFloat(key_, value);
      return *this;
    }

    template <typename T>
    typename std::enable_if<std::is_integral<T>::value &&
                                !std::is_same<T, bool>::value,
                            Value&>::type
    operator=(T value) {
      owner_.appendLiteral(key_, String(value));
      return *this;
    }

   private:
    JsonStringObject& owner_;
    const char* key_;
  };

  JsonStringObject() {
    output_.reserve(8192);
    appendRaw('{');
  }

  JsonStringObject(char* buffer, size_t capacity)
      : fixedBuffer_(buffer), fixedCapacity_(capacity) {
    if (fixedBuffer_ != nullptr && fixedCapacity_ != 0) {
      fixedBuffer_[0] = '\0';
    }
    appendRaw('{');
  }

  Value operator[](const char* key) { return Value(*this, key); }

  String finish() {
    appendRaw('}');
    if (fixedBuffer_ != nullptr || fieldCount_ < 100 ||
        output_.length() < 1000) {
      return "";
    }
    return output_;
  }

  bool finishFixed(size_t& length) {
    appendRaw('}');
    length = fixedLength_;
    return fixedBuffer_ != nullptr && !fixedOverflow_ &&
           fieldCount_ >= 100 && fixedLength_ >= 1000;
  }

 private:
  void beginValue(const char* key) {
    if (fieldCount_++ != 0) {
      appendRaw(',');
    }
    appendRaw('"');
    appendEscaped(key == nullptr ? "" : key);
    appendRaw("\":");
  }

  void appendEscaped(const char* value) {
    static constexpr char kHex[] = "0123456789abcdef";
    while (*value != '\0') {
      const uint8_t character = static_cast<uint8_t>(*value++);
      switch (character) {
        case '"':
          appendRaw("\\\"");
          break;
        case '\\':
          appendRaw("\\\\");
          break;
        case '\b':
          appendRaw("\\b");
          break;
        case '\f':
          appendRaw("\\f");
          break;
        case '\n':
          appendRaw("\\n");
          break;
        case '\r':
          appendRaw("\\r");
          break;
        case '\t':
          appendRaw("\\t");
          break;
        default:
          if (character < 0x20) {
            appendRaw("\\u00");
            appendRaw(kHex[(character >> 4) & 0x0f]);
            appendRaw(kHex[character & 0x0f]);
          } else {
            appendRaw(static_cast<char>(character));
          }
          break;
      }
    }
  }

  void appendString(const char* key, const String& value) {
    beginValue(key);
    appendRaw('"');
    appendEscaped(value.c_str());
    appendRaw('"');
  }

  void appendLiteral(const char* key, const String& value) {
    beginValue(key);
    appendRaw(value);
  }

  void appendLiteral(const char* key, const char* value) {
    beginValue(key);
    appendRaw(value);
  }

  void appendFloat(const char* key, double value) {
    String text(value, 6);
    while (text.endsWith("0")) {
      text.remove(text.length() - 1);
    }
    if (text.endsWith(".")) {
      text.remove(text.length() - 1);
    }
    appendLiteral(key, text);
  }

  void appendRaw(char value) {
    if (fixedBuffer_ == nullptr) {
      output_ += value;
      return;
    }
    if (fixedLength_ + 1 >= fixedCapacity_) {
      fixedOverflow_ = true;
      return;
    }
    fixedBuffer_[fixedLength_++] = value;
    fixedBuffer_[fixedLength_] = '\0';
  }

  void appendRaw(const char* value) {
    if (value == nullptr) return;
    while (*value != '\0') appendRaw(*value++);
  }

  void appendRaw(const String& value) { appendRaw(value.c_str()); }

  String output_;
  char* fixedBuffer_ = nullptr;
  size_t fixedCapacity_ = 0;
  size_t fixedLength_ = 0;
  bool fixedOverflow_ = false;
  uint16_t fieldCount_ = 0;
};

}  // namespace

template <typename TObject>
void writeConfigJson(TObject& root, const AppConfig& config,
                     bool includeSecrets);

bool ConfigStore::begin() {
  mutex_ = xSemaphoreCreateMutex();
  loadDefaults();
  // The complete browser configuration is close to NVS's per-page limit.
  // Rewriting it for sliders quickly exhausts the TC001's 20 KiB NVS
  // journal. SPIFFS has ample room and supports an atomic main/backup pair;
  // NVS remains reserved for the small credential vault.
  fileSystemReady_ = SPIFFS.begin(true);
  if (!preferences_.begin(kNamespace, false)) {
    return false;
  }

  bool loaded = false;
  bool migrateLegacy = false;
  if (fileSystemReady_) {
    const String stored = readConfigFile(kConfigPath);
    loaded = !stored.isEmpty() && deserialize(stored);
    if (!loaded) {
      const String backup = readConfigFile(kConfigBackupPath);
      if (!backup.isEmpty() && deserialize(backup)) {
        loaded = true;
        Serial.println(F("[config] Recovered SPIFFS configuration backup"));
      } else if (!stored.isEmpty()) {
        Serial.println(
            F("[config] Stored SPIFFS configuration was invalid"));
      }
    }
  }

  if (!loaded) {
    const String legacy = preferences_.getString(kConfigKey, "");
    loaded = !legacy.isEmpty() && deserialize(legacy);
    if (!loaded) {
      const String backup = preferences_.getString(kConfigBackupKey, "");
      if (!backup.isEmpty() && deserialize(backup)) {
        loaded = true;
        Serial.println(F("[config] Recovered legacy NVS backup"));
      } else if (!legacy.isEmpty()) {
        Serial.println(F("[config] Legacy NVS configuration was invalid"));
      }
    }
    if (loaded) {
      migrateLegacy = true;
      Serial.println(F("[config] Migrating settings from NVS to SPIFFS"));
    }
  }

  loadSecretVault();
  bool needsSave = migrateLegacy || migrationPending_ || !loaded;
  if (config_.apiKey.isEmpty()) {
    config_.apiKey = generateApiKey();
    needsSave = true;
  }
  if (needsSave && !save(config_)) {
    return false;
  }
  return fileSystemReady_;
}

void ConfigStore::loadSecretVault() {
  loadOrMigrateSecret(preferences_, kWifiPasswordKey, config_.wifiPassword);
  loadOrMigrateSecret(preferences_, kNetatmoClientIdKey,
                      config_.netatmoClientId);
  loadOrMigrateSecret(preferences_, kNetatmoClientSecretKey,
                      config_.netatmoClientSecret);
  loadOrMigrateSecret(preferences_, kNetatmoAccessTokenKey,
                      config_.netatmoAccessToken);
  loadOrMigrateSecret(preferences_, kNetatmoRefreshTokenKey,
                      config_.netatmoRefreshToken);
  const size_t garminCredentialBytes =
      preferences_.isKey(kGarminCredentialsKey)
          ? preferences_.getBytesLength(kGarminCredentialsKey)
          : 0;
  if (garminCredentialBytes != 0 && garminCredentialBytes <= 4096) {
    std::vector<uint8_t> encoded(garminCredentialBytes);
    if (preferences_.getBytes(kGarminCredentialsKey, encoded.data(),
                              encoded.size()) == encoded.size() &&
        decodeGarminCredentials(encoded.data(), encoded.size(),
                                config_.garminDomain,
                                config_.garminClientId,
                                config_.garminRefreshToken)) {
      // Loaded successfully. The bearer token is intentionally never part of
      // AppConfig and will be refreshed in the first explicit central Sync.
    } else {
      config_.garminClientId = "";
      config_.garminRefreshToken = "";
      Serial.println(F("[config] Ignored invalid Garmin credential blob"));
    }
  }
  loadOrMigrateSecret(preferences_, kMarketApiKeyKey, config_.marketApiKey);
  loadOrMigrateSecret(preferences_, kRemoteApiKeyKey, config_.apiKey);
  loadOrMigrateSecret(preferences_, kCalendarIcalUrlKey,
                      config_.calendarIcalUrl);
  loadOrMigrateSecret(preferences_, kCalendarIcalUrl2Key,
                      config_.calendarIcalUrl2);
  loadOrMigrateSecret(preferences_, kCalendarIcalUrl3Key,
                      config_.calendarIcalUrl3);
  loadOrMigrateSecret(preferences_, kCalendarIcalUrl4Key,
                      config_.calendarIcalUrl4);

  String loadedCalendarItems;
  const std::vector<CalendarItem> metadata =
      parseCalendarItems(config_.calendarItems, kMaximumCalendarItems);
  for (uint8_t index = 0; index < metadata.size(); ++index) {
    const CalendarItem& item = metadata[index];
    uint8_t vaultIndex = index;
    if (item.url.startsWith("@")) {
      const long parsed = strtol(item.url.c_str() + 1, nullptr, 10);
      if (parsed >= 0 && parsed < kMaximumCalendarItems) {
        vaultIndex = static_cast<uint8_t>(parsed);
      }
    }
    String url = item.url.startsWith("@")
                     ? preferences_.getString(
                           calendarSecretKey(vaultIndex).c_str(), "")
                     : item.url;
    appendCalendarItem(loadedCalendarItems, item.name, url, item.iconId);
  }
  if (loadedCalendarItems.isEmpty()) {
    const String urls[] = {config_.calendarIcalUrl,
                           config_.calendarIcalUrl2,
                           config_.calendarIcalUrl3,
                           config_.calendarIcalUrl4};
    const String names[] = {config_.calendarName1, config_.calendarName2,
                            config_.calendarName3, config_.calendarName4};
    for (uint8_t index = 0; index < 4; ++index) {
      appendCalendarItem(
          loadedCalendarItems,
          names[index].isEmpty() ? String("Calendar") : names[index],
          urls[index], config_.calendarIconId);
    }
  }
  config_.calendarItems = loadedCalendarItems;
}

bool ConfigStore::saveSecretVault(const AppConfig& config) {
  bool calendarsSaved = true;
  const std::vector<CalendarItem> calendarItems =
      parseCalendarItems(config.calendarItems, kMaximumCalendarItems);
  for (uint8_t index = 0; index < kMaximumCalendarItems; ++index) {
    const String value =
        index < calendarItems.size() ? calendarItems[index].url : String();
    calendarsSaved =
        putSecret(preferences_, calendarSecretKey(index).c_str(), value) &&
        calendarsSaved;
  }
  return calendarsSaved &&
         putSecret(preferences_, kWifiPasswordKey, config.wifiPassword) &&
         putSecret(preferences_, kNetatmoClientIdKey,
                   config.netatmoClientId) &&
         putSecret(preferences_, kNetatmoClientSecretKey,
                   config.netatmoClientSecret) &&
         putSecret(preferences_, kNetatmoAccessTokenKey,
                   config.netatmoAccessToken) &&
         putSecret(preferences_, kNetatmoRefreshTokenKey,
                   config.netatmoRefreshToken) &&
         putGarminCredentials(preferences_, config.garminDomain,
                              config.garminClientId,
                              config.garminRefreshToken) &&
         putSecret(preferences_, kMarketApiKeyKey, config.marketApiKey) &&
         putSecret(preferences_, kRemoteApiKeyKey, config.apiKey) &&
         putSecret(preferences_, kCalendarIcalUrlKey,
                   config.calendarIcalUrl) &&
         putSecret(preferences_, kCalendarIcalUrl2Key,
                   config.calendarIcalUrl2) &&
         putSecret(preferences_, kCalendarIcalUrl3Key,
                   config.calendarIcalUrl3) &&
         putSecret(preferences_, kCalendarIcalUrl4Key,
                   config.calendarIcalUrl4);
}

void ConfigStore::loadDefaults() {
  config_ = AppConfig{};
}

AppConfig ConfigStore::snapshot() const {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  const AppConfig copy = config_;
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  return copy;
}

void ConfigStore::copyTo(AppConfig& destination) const {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  destination = config_;
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
}

bool ConfigStore::save(AppConfig& input) {
  ScopedTlsHeapLease settingsHeap;
  // The caller owns the candidate, so normalize it in place. Avoiding another
  // deep copy of all String fields leaves enough contiguous heap for a portal
  // save even while icon and online-data caches are populated.
  normalize(input);
  // A full twelve-alarm schedule, six bounded RTTTL slots and sixteen Travel
  // rows can exceed 16 KiB after JSON escaping. Borrowing 20 KiB from the
  // private 40--60 KiB TLS arena is safe here: the settings lease excludes
  // every provider handshake and the block is freed before the lease is
  // released. Keep deterministic headroom so adding a stop cannot turn a
  // seemingly unrelated settings save into an overflow.
  constexpr size_t kPersistenceCapacity = 20U * 1024U;
  std::unique_ptr<char, decltype(&freeTlsScratch)> persistenceBuffer(
      static_cast<char*>(allocateTlsScratch(kPersistenceCapacity)),
      &freeTlsScratch);
  if (!persistenceBuffer) {
    Serial.println(F("[config] Could not allocate settings buffer"));
    return false;
  }
  JsonStringObject persistenceRoot(persistenceBuffer.get(),
                                   kPersistenceCapacity);
  writeConfigJson(persistenceRoot, input, false);
  size_t persistenceLength = 0;
  const bool serialized = persistenceRoot.finishFixed(persistenceLength);
  if (!serialized) {
    Serial.println(F("[config] Fixed settings buffer overflowed"));
  }
  // Commit durable state before publishing it to running services. A failed
  // save must not silently change Wi-Fi or app state in RAM.
  const bool configWritten =
      fileSystemReady_ && serialized &&
      writeConfigFile(persistenceBuffer.get(), persistenceLength);
  const bool secretsWritten = configWritten && saveSecretVault(input);
  if (secretsWritten && configWritten) {
    if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
    config_ = input;
    ++revision_;
    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
  }
  if (configWritten) {
    // Remove the legacy multi-kilobyte values once an atomic file is safely
    // committed. This leaves NVS with only infrequently-changing secrets.
    if (preferences_.isKey(kConfigKey)) {
      preferences_.remove(kConfigKey);
    }
    if (preferences_.isKey(kConfigBackupKey)) {
      preferences_.remove(kConfigBackupKey);
    }
  }
  if (secretsWritten && configWritten) {
    migrationPending_ = false;
  }
  return secretsWritten && configWritten;
}

bool ConfigStore::deserialize(const String& json) {
  // Parse the flat settings object in-place.  String values point into this
  // mutable copy until updateFromJson() copies them into AppConfig, avoiding
  // another large ArduinoJson string pool during boot and migration.
  String mutableJson = json;
  DynamicJsonDocument document(6144);
  const DeserializationError parseError =
      deserializeJson(document, mutableJson.begin(), mutableJson.length());
  if (parseError) {
    return false;
  }
  const JsonObjectConst root = document.as<JsonObjectConst>();
  if (!root.containsKey("schema_version") ||
      !root.containsKey("device_name") ||
      !root.containsKey("wifi_ssid")) {
    return false;
  }

  String error;
  const uint16_t storedSchema = root["schema_version"] | 0;
  if (!updateFromJson(root, error)) {
    return false;
  }
  migrationPending_ = migrationPending_ || storedSchema < 28;
  return true;
}

template <typename TObject>
void writeConfigJson(TObject& root, const AppConfig& config,
                     bool includeSecrets) {
  // Temporarily serialize the supplied object rather than config_.
  root["schema_version"] = config.schemaVersion;
  root["device_name"] = config.deviceName;
  root["language"] = config.language;
  root["timezone"] = config.timezone;
  root["orientation_180"] = config.orientation180;
  root["clock_24_hour"] = config.clock24Hour;
  root["clock_timezone_2"] = config.clockTimezone2;
  root["clock_timezone_3"] = config.clockTimezone3;
  root["clock_timezone_4"] = config.clockTimezone4;
  root["clock_timezone_seconds"] = config.clockTimezoneSeconds;
  root["clock_timezone_items"] = config.clockTimezoneItems;
  root["alarm_items"] = config.alarmItems;
  root["wifi_ssid"] = config.wifiSsid;
  root["wifi_password"] =
      includeSecrets ? config.wifiPassword : String("");

  root["auto_brightness"] = config.autoBrightness;
  root["manual_brightness"] = config.manualBrightness;
  root["minimum_brightness"] = config.minimumBrightness;
  root["maximum_brightness"] = config.maximumBrightness;
  root["ldr_dark"] = config.ldrDark;
  root["ldr_bright"] = config.ldrBright;
  root["brightness_gamma"] = config.brightnessGamma;
  root["battery_empty_raw"] = config.batteryEmptyRaw;
  root["battery_full_raw"] = config.batteryFullRaw;
  root["battery_saver_enabled"] = config.batterySaverEnabled;
  root["battery_saver_threshold"] = config.batterySaverThreshold;
  root["battery_saver_brightness"] = config.batterySaverBrightness;
  root["battery_auto_low_enabled"] = config.batteryAutoLowEnabled;

  root["time_enabled"] = config.timeEnabled;
  root["climate_enabled"] = config.climateEnabled;
  root["netatmo_enabled"] = config.netatmoEnabled;
  root["climate_provider"] = config.climateProvider;
  root["rss_enabled"] = config.rssEnabled;
  root["battery_enabled"] = config.batteryEnabled;
  root["market_enabled"] = config.marketEnabled;
  root["energy_enabled"] = config.energyEnabled;
  root["calendar_enabled"] = config.calendarEnabled;
  root["travel_enabled"] = config.travelEnabled;
  root["reddit_enabled"] = config.redditEnabled;
  root["activity_enabled"] = config.activityEnabled;
  root["counter_enabled"] = config.counterEnabled;
  root["time_animation"] = config.timeAnimation;
  root["climate_animation"] = config.climateAnimation;
  root["netatmo_animation"] = config.netatmoAnimation;
  root["rss_animation"] = config.rssAnimation;
  root["battery_animation"] = config.batteryAnimation;
  root["market_animation"] = config.marketAnimation;
  root["energy_animation"] = config.energyAnimation;
  root["calendar_animation"] = config.calendarAnimation;
  root["travel_animation"] = config.travelAnimation;
  root["reddit_animation"] = config.redditAnimation;
  root["activity_animation"] = config.activityAnimation;
  root["app_duration_seconds"] = config.appDurationSeconds;
  root["maximum_app_seconds"] = config.maximumAppSeconds;
  root["scroll_step_ms"] = config.scrollStepMs;
  root["maximum_message_chars"] = config.maximumMessageChars;
  root["sync_every_loops"] = config.syncEveryLoops;
  root["time_color"] = htmlColor(config.timeColor);
  root["climate_color"] = htmlColor(config.climateColor);
  root["netatmo_color"] = htmlColor(config.netatmoColor);
  root["rss_color"] = htmlColor(config.rssColor);
  root["battery_color"] = htmlColor(config.batteryColor);
  root["calendar_color"] = htmlColor(config.calendarColor);
  root["travel_color"] = htmlColor(config.travelColor);
  root["reddit_color"] = htmlColor(config.redditColor);
  root["activity_color"] = htmlColor(config.activityColor);
  root["time_icon_id"] = config.timeIconId;
  root["climate_icon_id"] = config.climateIconId;
  root["netatmo_icon_id"] = config.netatmoIconId;
  root["rss_icon_id_1"] = config.rssIconId1;
  root["rss_icon_id_2"] = config.rssIconId2;
  root["market_icon_id"] = config.marketIconId;
  root["battery_icon_id"] = config.batteryIconId;
  root["battery_icon_high_id"] = config.batteryIconHighId;
  root["battery_icon_medium_id"] = config.batteryIconMediumId;
  root["battery_icon_low_id"] = config.batteryIconLowId;
  root["climate_icon_cold_id"] = config.climateIconColdId;
  root["climate_icon_hot_id"] = config.climateIconHotId;
  root["netatmo_icon_cold_id"] = config.netatmoIconColdId;
  root["netatmo_icon_hot_id"] = config.netatmoIconHotId;
  root["netatmo_icon_sunny_id"] = config.netatmoIconSunnyId;
  root["netatmo_icon_rain_id"] = config.netatmoIconRainId;
  root["climate_temp_low_icon_id"] =
      config.climateTemperatureLowIconId;
  root["climate_temp_medium_icon_id"] =
      config.climateTemperatureMediumIconId;
  root["climate_temp_high_icon_id"] =
      config.climateTemperatureHighIconId;
  root["climate_humidity_low_icon_id"] =
      config.climateHumidityLowIconId;
  root["climate_humidity_medium_icon_id"] =
      config.climateHumidityMediumIconId;
  root["climate_humidity_high_icon_id"] =
      config.climateHumidityHighIconId;
  root["climate_wind_icon_id"] = config.climateWindIconId;
  root["climate_rain_icon_id"] = config.climateRainIconId;
  root["netatmo_temp_low_icon_id"] =
      config.netatmoTemperatureLowIconId;
  root["netatmo_temp_medium_icon_id"] =
      config.netatmoTemperatureMediumIconId;
  root["netatmo_temp_high_icon_id"] =
      config.netatmoTemperatureHighIconId;
  root["netatmo_humidity_low_icon_id"] =
      config.netatmoHumidityLowIconId;
  root["netatmo_humidity_medium_icon_id"] =
      config.netatmoHumidityMediumIconId;
  root["netatmo_humidity_high_icon_id"] =
      config.netatmoHumidityHighIconId;
  root["market_icon_up_id"] = config.marketIconUpId;
  root["market_icon_down_id"] = config.marketIconDownId;
  root["energy_icon_low_id"] = config.energyIconLowId;
  root["energy_icon_average_id"] = config.energyIconAverageId;
  root["energy_icon_high_id"] = config.energyIconHighId;
  root["rss_fallback_icon_id"] = config.rssFallbackIconId;
  root["calendar_icon_id"] = config.calendarIconId;
  root["reddit_icon_id"] = config.redditIconId;
  root["activity_icon_id"] = config.activityIconId;
  root["activity_show_steps"] = config.activityShowSteps;
  root["activity_show_daily_distance"] =
      config.activityShowDailyDistance;
  root["activity_show_latest_distance"] =
      config.activityShowLatestDistance;
  root["activity_show_latest_duration"] =
      config.activityShowLatestDuration;
  root["activity_show_latest_calories"] =
      config.activityShowLatestCalories;
  root["counter_icon_id"] = config.counterIconId;
  root["counter_mode"] = config.counterMode;
  root["counter_duration_seconds"] = config.counterDurationSeconds;

  root["rss_url_1"] = config.rssUrl1;
  root["rss_url_2"] = config.rssUrl2;
  root["rss_feed_items"] = config.rssFeedItems;
  root["rss_headline_count"] = config.rssHeadlineCount;
  root["rss_refresh_minutes"] = config.rssRefreshMinutes;
  root["rss_beep_on_new"] = config.rssBeepOnNew;

  root["netatmo_client_id"] =
      includeSecrets ? config.netatmoClientId : String("");
  root["netatmo_client_secret"] =
      includeSecrets ? config.netatmoClientSecret : String("");
  root["netatmo_access_token"] =
      includeSecrets ? config.netatmoAccessToken : String("");
  root["netatmo_refresh_token"] =
      includeSecrets ? config.netatmoRefreshToken : String("");
  root["netatmo_device_id"] = config.netatmoDeviceId;
  root["netatmo_module_id"] = config.netatmoModuleId;
  root["netatmo_module_name"] = config.netatmoModuleName;
  root["netatmo_refresh_minutes"] = config.netatmoRefreshMinutes;
  root["climate_rain_alert_enabled"] = config.climateRainAlertEnabled;

  root["market_provider"] = config.marketProvider;
  root["market_api_key"] =
      includeSecrets ? config.marketApiKey : String("");
  root["market_symbols"] = config.marketSymbols;
  root["market_ticker_icons"] = config.marketTickerIcons;
  root["market_items"] = config.marketItems;
  root["market_refresh_minutes"] = config.marketRefreshMinutes;
  root["market_chart_enabled"] = config.marketChartEnabled;
  root["market_chart_range"] = config.marketChartRange;

  root["energy_country"] = config.energyCountry;
  root["energy_refresh_minutes"] = config.energyRefreshMinutes;
  root["energy_include_vat"] = config.energyIncludeVat;
  root["energy_vat_percent"] = config.energyVatPercent;
  root["energy_beep_on_change"] = config.energyBeepOnChange;
  root["energy_chart_enabled"] = config.energyChartEnabled;
  root["energy_chart_range"] = config.energyChartRange;


  root["calendar_items"] =
      includeSecrets ? config.calendarItems
                     : redactedCalendarItems(config.calendarItems);
  root["calendar_ical_url"] =
      includeSecrets ? config.calendarIcalUrl : String("");
  root["calendar_name_1"] = config.calendarName1;
  root["calendar_ical_url_2"] =
      includeSecrets ? config.calendarIcalUrl2 : String("");
  root["calendar_name_2"] = config.calendarName2;
  root["calendar_ical_url_3"] =
      includeSecrets ? config.calendarIcalUrl3 : String("");
  root["calendar_name_3"] = config.calendarName3;
  root["calendar_ical_url_4"] =
      includeSecrets ? config.calendarIcalUrl4 : String("");
  root["calendar_name_4"] = config.calendarName4;
  root["calendar_refresh_minutes"] = config.calendarRefreshMinutes;
  root["calendar_maximum_events"] = config.calendarMaximumEvents;
  root["calendar_alerts_enabled"] = config.calendarAlertsEnabled;
  root["calendar_tone"] = config.calendarTone;

  root["travel_stop_items"] = config.travelStopItems;

  root["reddit_subreddits"] = config.redditSubreddits;
  root["reddit_items"] = config.redditItems;
  root["reddit_period"] = config.redditPeriod;
  root["reddit_post_count"] = config.redditPostCount;
  root["reddit_refresh_minutes"] = config.redditRefreshMinutes;

  root["garmin_domain"] = config.garminDomain;
  root["garmin_client_id"] =
      includeSecrets ? config.garminClientId : String("");
  root["garmin_refresh_token"] =
      includeSecrets ? config.garminRefreshToken : String("");
  root["garmin_display_name"] = config.garminDisplayName;

  root["sleep_enabled"] = config.sleepEnabled;
  root["sleep_start"] = config.sleepStart;
  root["sleep_end"] = config.sleepEnd;
  root["sleep_idle_enabled"] = config.sleepIdleEnabled;
  root["sleep_idle_minutes"] = config.sleepIdleMinutes;
  root["sleep_display_mode"] = config.sleepDisplayMode;
  root["button_wake_minutes"] = config.buttonWakeMinutes;
  root["button_short_left"] = config.buttonShortLeft;
  root["button_short_middle"] = config.buttonShortMiddle;
  root["button_short_right"] = config.buttonShortRight;
  root["button_long_left"] = config.buttonLongLeft;
  root["button_long_middle"] = config.buttonLongMiddle;
  root["button_long_right"] = config.buttonLongRight;
  root["remote_wakes_display"] = config.remoteWakesDisplay;
  root["hourly_chime_enabled"] = config.hourlyChimeEnabled;
  root["hourly_chime_start"] = config.hourlyChimeStart;
  root["hourly_chime_end"] = config.hourlyChimeEnd;
  root["news_tone"] = config.newsTone;
  root["climate_tone"] = config.climateTone;
  root["alarm_tone"] = config.alarmTone;
  root["counter_tone"] = config.counterTone;
  root["hourly_tone"] = config.hourlyTone;
  root["notification_tone"] = config.notificationTone;
  root["button_tone"] = config.buttonTone;
  root["energy_tone"] = config.energyTone;
  root["disabled_builtin_tones"] = config.disabledBuiltinTones;
  root["buzzer_volume"] = config.buzzerVolume;
  root["buzzer_pitch_percent"] = config.buzzerPitchPercent;
  root["buzzer_high_note_boost_percent"] =
      config.buzzerHighNoteBoostPercent;
  root["custom_rtttl_1"] = config.customRtttl1;
  root["custom_rtttl_2"] = config.customRtttl2;
  root["custom_rtttl_3"] = config.customRtttl3;
  root["custom_rtttl_4"] = config.customRtttl4;
  root["custom_rtttl_5"] = config.customRtttl5;
  root["custom_rtttl_6"] = config.customRtttl6;

  root["remote_enabled"] = config.remoteEnabled;
  root["remote_timeout_ms"] = config.remoteTimeoutMs;
  root["remote_source_ip"] = config.remoteSourceIp;
  root["api_key"] = includeSecrets ? config.apiKey : String("");
}

bool ConfigStore::updateFromJson(JsonObjectConst object, String& error,
                                 AppConfig* result, bool publish) {
  // This routine has a long chain of JSON assignments and normalization
  // temporaries. Keeping the growing AppConfig itself off loopTask's 8 KiB
  // stack prevents browser saves from tripping the ESP32 stack guard.
  std::unique_ptr<AppConfig> nextStorage;
  if (result == nullptr) {
    nextStorage.reset(new AppConfig(snapshot()));
  } else {
    *result = snapshot();
  }
  AppConfig& next = result == nullptr ? *nextStorage : *result;

  readIfPresent(object, "schema_version", next.schemaVersion);
  readIfPresent(object, "device_name", next.deviceName);
  readIfPresent(object, "language", next.language);
  readIfPresent(object, "timezone", next.timezone);
  readIfPresent(object, "orientation_180", next.orientation180);
  readIfPresent(object, "clock_24_hour", next.clock24Hour);
  readIfPresent(object, "clock_timezone_2", next.clockTimezone2);
  readIfPresent(object, "clock_timezone_3", next.clockTimezone3);
  readIfPresent(object, "clock_timezone_4", next.clockTimezone4);
  readIfPresent(object, "clock_timezone_seconds",
                next.clockTimezoneSeconds);
  readIfPresent(object, "clock_timezone_items", next.clockTimezoneItems);
  readIfPresent(object, "alarm_items", next.alarmItems);
  readIfPresent(object, "wifi_ssid", next.wifiSsid);
  readSecretIfPresent(object, "wifi_password", next.wifiPassword);

  readIfPresent(object, "auto_brightness", next.autoBrightness);
  readIfPresent(object, "manual_brightness", next.manualBrightness);
  readIfPresent(object, "minimum_brightness", next.minimumBrightness);
  readIfPresent(object, "maximum_brightness", next.maximumBrightness);
  readIfPresent(object, "ldr_dark", next.ldrDark);
  readIfPresent(object, "ldr_bright", next.ldrBright);
  readIfPresent(object, "brightness_gamma", next.brightnessGamma);
  readIfPresent(object, "battery_empty_raw", next.batteryEmptyRaw);
  readIfPresent(object, "battery_full_raw", next.batteryFullRaw);
  readIfPresent(object, "battery_saver_enabled", next.batterySaverEnabled);
  readIfPresent(object, "battery_saver_threshold",
                next.batterySaverThreshold);
  readIfPresent(object, "battery_saver_brightness",
                next.batterySaverBrightness);
  readIfPresent(object, "battery_auto_low_enabled",
                next.batteryAutoLowEnabled);

  readIfPresent(object, "time_enabled", next.timeEnabled);
  readIfPresent(object, "climate_enabled", next.climateEnabled);
  readIfPresent(object, "netatmo_enabled", next.netatmoEnabled);
  readIfPresent(object, "climate_provider", next.climateProvider);
  readIfPresent(object, "rss_enabled", next.rssEnabled);
  readIfPresent(object, "battery_enabled", next.batteryEnabled);
  readIfPresent(object, "market_enabled", next.marketEnabled);
  readIfPresent(object, "energy_enabled", next.energyEnabled);
  readIfPresent(object, "calendar_enabled", next.calendarEnabled);
  readIfPresent(object, "travel_enabled", next.travelEnabled);
  readIfPresent(object, "reddit_enabled", next.redditEnabled);
  readIfPresent(object, "activity_enabled", next.activityEnabled);
  readIfPresent(object, "counter_enabled", next.counterEnabled);
  readIfPresent(object, "time_animation", next.timeAnimation);
  readIfPresent(object, "climate_animation", next.climateAnimation);
  readIfPresent(object, "netatmo_animation", next.netatmoAnimation);
  readIfPresent(object, "rss_animation", next.rssAnimation);
  readIfPresent(object, "battery_animation", next.batteryAnimation);
  readIfPresent(object, "market_animation", next.marketAnimation);
  readIfPresent(object, "energy_animation", next.energyAnimation);
  readIfPresent(object, "calendar_animation", next.calendarAnimation);
  readIfPresent(object, "travel_animation", next.travelAnimation);
  readIfPresent(object, "reddit_animation", next.redditAnimation);
  readIfPresent(object, "activity_animation", next.activityAnimation);
  readIfPresent(object, "app_duration_seconds", next.appDurationSeconds);
  readIfPresent(object, "maximum_app_seconds", next.maximumAppSeconds);
  readIfPresent(object, "scroll_step_ms", next.scrollStepMs);
  readIfPresent(object, "maximum_message_chars", next.maximumMessageChars);
  readIfPresent(object, "sync_every_loops", next.syncEveryLoops);
  if (!object["time_color"].isNull()) {
    next.timeColor =
        parseHtmlColor(object["time_color"].as<String>(), next.timeColor);
  }
  if (!object["climate_color"].isNull()) {
    next.climateColor =
        parseHtmlColor(object["climate_color"].as<String>(), next.climateColor);
  }
  if (!object["netatmo_color"].isNull()) {
    next.netatmoColor =
        parseHtmlColor(object["netatmo_color"].as<String>(), next.netatmoColor);
  }
  if (!object["rss_color"].isNull()) {
    next.rssColor =
        parseHtmlColor(object["rss_color"].as<String>(), next.rssColor);
  }
  if (!object["battery_color"].isNull()) {
    next.batteryColor =
        parseHtmlColor(object["battery_color"].as<String>(), next.batteryColor);
  }
  if (!object["calendar_color"].isNull()) {
    next.calendarColor = parseHtmlColor(object["calendar_color"].as<String>(),
                                        next.calendarColor);
  }
  if (!object["travel_color"].isNull()) {
    next.travelColor =
        parseHtmlColor(object["travel_color"].as<String>(), next.travelColor);
  }
  if (!object["reddit_color"].isNull()) {
    next.redditColor =
        parseHtmlColor(object["reddit_color"].as<String>(), next.redditColor);
  }
  if (!object["activity_color"].isNull()) {
    next.activityColor = parseHtmlColor(
        object["activity_color"].as<String>(), next.activityColor);
  }
  readIfPresent(object, "time_icon_id", next.timeIconId);
  readIfPresent(object, "climate_icon_id", next.climateIconId);
  readIfPresent(object, "netatmo_icon_id", next.netatmoIconId);
  readIfPresent(object, "rss_icon_id_1", next.rssIconId1);
  readIfPresent(object, "rss_icon_id_2", next.rssIconId2);
  readIfPresent(object, "market_icon_id", next.marketIconId);
  readIfPresent(object, "battery_icon_id", next.batteryIconId);
  readIfPresent(object, "battery_icon_high_id", next.batteryIconHighId);
  readIfPresent(object, "battery_icon_medium_id",
                next.batteryIconMediumId);
  readIfPresent(object, "battery_icon_low_id", next.batteryIconLowId);
  readIfPresent(object, "climate_icon_cold_id", next.climateIconColdId);
  readIfPresent(object, "climate_icon_hot_id", next.climateIconHotId);
  readIfPresent(object, "netatmo_icon_cold_id", next.netatmoIconColdId);
  readIfPresent(object, "netatmo_icon_hot_id", next.netatmoIconHotId);
  readIfPresent(object, "netatmo_icon_sunny_id", next.netatmoIconSunnyId);
  readIfPresent(object, "netatmo_icon_rain_id", next.netatmoIconRainId);
  readIfPresent(object, "climate_temp_low_icon_id",
                next.climateTemperatureLowIconId);
  readIfPresent(object, "climate_temp_medium_icon_id",
                next.climateTemperatureMediumIconId);
  readIfPresent(object, "climate_temp_high_icon_id",
                next.climateTemperatureHighIconId);
  readIfPresent(object, "climate_humidity_low_icon_id",
                next.climateHumidityLowIconId);
  readIfPresent(object, "climate_humidity_medium_icon_id",
                next.climateHumidityMediumIconId);
  readIfPresent(object, "climate_humidity_high_icon_id",
                next.climateHumidityHighIconId);
  readIfPresent(object, "climate_wind_icon_id", next.climateWindIconId);
  readIfPresent(object, "climate_rain_icon_id", next.climateRainIconId);
  readIfPresent(object, "netatmo_temp_low_icon_id",
                next.netatmoTemperatureLowIconId);
  readIfPresent(object, "netatmo_temp_medium_icon_id",
                next.netatmoTemperatureMediumIconId);
  readIfPresent(object, "netatmo_temp_high_icon_id",
                next.netatmoTemperatureHighIconId);
  readIfPresent(object, "netatmo_humidity_low_icon_id",
                next.netatmoHumidityLowIconId);
  readIfPresent(object, "netatmo_humidity_medium_icon_id",
                next.netatmoHumidityMediumIconId);
  readIfPresent(object, "netatmo_humidity_high_icon_id",
                next.netatmoHumidityHighIconId);
  readIfPresent(object, "market_icon_up_id", next.marketIconUpId);
  readIfPresent(object, "market_icon_down_id", next.marketIconDownId);
  readIfPresent(object, "energy_icon_low_id", next.energyIconLowId);
  readIfPresent(object, "energy_icon_average_id", next.energyIconAverageId);
  readIfPresent(object, "energy_icon_high_id", next.energyIconHighId);
  readIfPresent(object, "rss_fallback_icon_id", next.rssFallbackIconId);
  readIfPresent(object, "calendar_icon_id", next.calendarIconId);
  readIfPresent(object, "reddit_icon_id", next.redditIconId);
  readIfPresent(object, "activity_icon_id", next.activityIconId);
  readIfPresent(object, "activity_show_steps", next.activityShowSteps);
  readIfPresent(object, "activity_show_daily_distance",
                next.activityShowDailyDistance);
  readIfPresent(object, "activity_show_latest_distance",
                next.activityShowLatestDistance);
  readIfPresent(object, "activity_show_latest_duration",
                next.activityShowLatestDuration);
  readIfPresent(object, "activity_show_latest_calories",
                next.activityShowLatestCalories);
  readIfPresent(object, "counter_icon_id", next.counterIconId);
  readIfPresent(object, "counter_mode", next.counterMode);
  readIfPresent(object, "counter_duration_seconds",
                next.counterDurationSeconds);

  readIfPresent(object, "rss_url_1", next.rssUrl1);
  readIfPresent(object, "rss_url_2", next.rssUrl2);
  readIfPresent(object, "rss_feed_items", next.rssFeedItems);
  readIfPresent(object, "rss_headline_count", next.rssHeadlineCount);
  readIfPresent(object, "rss_refresh_minutes", next.rssRefreshMinutes);
  readIfPresent(object, "rss_beep_on_new", next.rssBeepOnNew);

  readSecretIfPresent(object, "netatmo_client_id", next.netatmoClientId);
  readSecretIfPresent(object, "netatmo_client_secret",
                      next.netatmoClientSecret);
  readSecretIfPresent(object, "netatmo_access_token",
                      next.netatmoAccessToken);
  readSecretIfPresent(object, "netatmo_refresh_token",
                      next.netatmoRefreshToken);
  readIfPresent(object, "netatmo_device_id", next.netatmoDeviceId);
  readIfPresent(object, "netatmo_module_id", next.netatmoModuleId);
  readIfPresent(object, "netatmo_module_name", next.netatmoModuleName);
  readIfPresent(object, "netatmo_refresh_minutes",
                next.netatmoRefreshMinutes);
  readIfPresent(object, "climate_rain_alert_enabled",
                next.climateRainAlertEnabled);

  readIfPresent(object, "market_provider", next.marketProvider);
  readSecretIfPresent(object, "market_api_key", next.marketApiKey);
  readIfPresent(object, "market_symbols", next.marketSymbols);
  readIfPresent(object, "market_ticker_icons", next.marketTickerIcons);
  readIfPresent(object, "market_items", next.marketItems);
  readIfPresent(object, "market_refresh_minutes", next.marketRefreshMinutes);
  readIfPresent(object, "market_chart_enabled", next.marketChartEnabled);
  readIfPresent(object, "market_chart_range", next.marketChartRange);

  readIfPresent(object, "energy_country", next.energyCountry);
  readIfPresent(object, "energy_refresh_minutes", next.energyRefreshMinutes);
  readIfPresent(object, "energy_include_vat", next.energyIncludeVat);
  readIfPresent(object, "energy_vat_percent", next.energyVatPercent);
  readIfPresent(object, "energy_beep_on_change", next.energyBeepOnChange);
  readIfPresent(object, "energy_chart_enabled", next.energyChartEnabled);
  readIfPresent(object, "energy_chart_range", next.energyChartRange);


  if (!object["calendar_items"].isNull()) {
    next.calendarItems = resolveCalendarItems(
        object["calendar_items"].as<String>(), next.calendarItems);
  }
  readSecretIfPresent(object, "calendar_ical_url", next.calendarIcalUrl);
  readIfPresent(object, "calendar_name_1", next.calendarName1);
  readSecretIfPresent(object, "calendar_ical_url_2", next.calendarIcalUrl2);
  readIfPresent(object, "calendar_name_2", next.calendarName2);
  readSecretIfPresent(object, "calendar_ical_url_3", next.calendarIcalUrl3);
  readIfPresent(object, "calendar_name_3", next.calendarName3);
  readSecretIfPresent(object, "calendar_ical_url_4", next.calendarIcalUrl4);
  readIfPresent(object, "calendar_name_4", next.calendarName4);
  readIfPresent(object, "calendar_refresh_minutes",
                next.calendarRefreshMinutes);
  readIfPresent(object, "calendar_maximum_events",
                next.calendarMaximumEvents);
  readIfPresent(object, "calendar_alerts_enabled",
                next.calendarAlertsEnabled);
  readIfPresent(object, "calendar_tone", next.calendarTone);

  readIfPresent(object, "travel_stop_items", next.travelStopItems);

  readIfPresent(object, "reddit_subreddits", next.redditSubreddits);
  readIfPresent(object, "reddit_items", next.redditItems);
  readIfPresent(object, "reddit_period", next.redditPeriod);
  readIfPresent(object, "reddit_post_count", next.redditPostCount);
  readIfPresent(object, "reddit_refresh_minutes",
                next.redditRefreshMinutes);
  readIfPresent(object, "garmin_domain", next.garminDomain);
  readSecretIfPresent(object, "garmin_client_id", next.garminClientId);
  readSecretIfPresent(object, "garmin_refresh_token",
                      next.garminRefreshToken);
  readIfPresent(object, "garmin_display_name", next.garminDisplayName);

  readIfPresent(object, "sleep_enabled", next.sleepEnabled);
  readIfPresent(object, "sleep_start", next.sleepStart);
  readIfPresent(object, "sleep_end", next.sleepEnd);
  readIfPresent(object, "sleep_idle_enabled", next.sleepIdleEnabled);
  readIfPresent(object, "sleep_idle_minutes", next.sleepIdleMinutes);
  readIfPresent(object, "sleep_display_mode", next.sleepDisplayMode);
  readIfPresent(object, "button_wake_minutes", next.buttonWakeMinutes);
  readIfPresent(object, "button_short_left", next.buttonShortLeft);
  readIfPresent(object, "button_short_middle", next.buttonShortMiddle);
  readIfPresent(object, "button_short_right", next.buttonShortRight);
  readIfPresent(object, "button_long_left", next.buttonLongLeft);
  readIfPresent(object, "button_long_middle", next.buttonLongMiddle);
  readIfPresent(object, "button_long_right", next.buttonLongRight);
  readIfPresent(object, "remote_wakes_display", next.remoteWakesDisplay);
  readIfPresent(object, "hourly_chime_enabled", next.hourlyChimeEnabled);
  readIfPresent(object, "hourly_chime_start", next.hourlyChimeStart);
  readIfPresent(object, "hourly_chime_end", next.hourlyChimeEnd);
  readIfPresent(object, "news_tone", next.newsTone);
  readIfPresent(object, "climate_tone", next.climateTone);
  readIfPresent(object, "alarm_tone", next.alarmTone);
  readIfPresent(object, "counter_tone", next.counterTone);
  readIfPresent(object, "hourly_tone", next.hourlyTone);
  readIfPresent(object, "notification_tone", next.notificationTone);
  readIfPresent(object, "button_tone", next.buttonTone);
  readIfPresent(object, "energy_tone", next.energyTone);
  readIfPresent(object, "disabled_builtin_tones",
                next.disabledBuiltinTones);
  readIfPresent(object, "buzzer_volume", next.buzzerVolume);
  readIfPresent(object, "buzzer_pitch_percent", next.buzzerPitchPercent);
  readIfPresent(object, "buzzer_high_note_boost_percent",
                next.buzzerHighNoteBoostPercent);
  readIfPresent(object, "custom_rtttl_1", next.customRtttl1);
  readIfPresent(object, "custom_rtttl_2", next.customRtttl2);
  readIfPresent(object, "custom_rtttl_3", next.customRtttl3);
  readIfPresent(object, "custom_rtttl_4", next.customRtttl4);
  readIfPresent(object, "custom_rtttl_5", next.customRtttl5);
  readIfPresent(object, "custom_rtttl_6", next.customRtttl6);

  readIfPresent(object, "remote_enabled", next.remoteEnabled);
  readIfPresent(object, "remote_timeout_ms", next.remoteTimeoutMs);
  readIfPresent(object, "remote_source_ip", next.remoteSourceIp);
  readSecretIfPresent(object, "api_key", next.apiKey);

  if (object["clear_wifi_password"] == true) {
    next.wifiPassword = "";
  }
  if (object["clear_netatmo_credentials"] == true) {
    next.netatmoClientId = "";
    next.netatmoClientSecret = "";
    next.netatmoAccessToken = "";
    next.netatmoRefreshToken = "";
  }
  if (object["clear_garmin_credentials"] == true) {
    next.garminClientId = "";
    next.garminRefreshToken = "";
    next.garminDisplayName = "";
    next.activityEnabled = false;
  }
  if (object["clear_market_api_key"] == true) {
    next.marketApiKey = "";
  }
  if (object["clear_calendar_ical_url"] == true) {
    next.calendarIcalUrl = "";
  }
  if (object["clear_calendar_ical_url_2"] == true) {
    next.calendarIcalUrl2 = "";
  }
  if (object["clear_calendar_ical_url_3"] == true) {
    next.calendarIcalUrl3 = "";
  }
  if (object["clear_calendar_ical_url_4"] == true) {
    next.calendarIcalUrl4 = "";
  }

  normalize(next);
  if (next.ldrDark >= next.ldrBright) {
    error = "ldr_dark must be smaller than ldr_bright";
    return false;
  }
  if (next.minimumBrightness > next.maximumBrightness) {
    error = "minimum_brightness must not exceed maximum_brightness";
    return false;
  }
  if (next.deviceName.isEmpty()) {
    error = "device_name must not be empty";
    return false;
  }

  if (!publish) {
    return true;
  }
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  config_ = next;
  ++revision_;
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  return true;
}

void ConfigStore::toJson(JsonObject root, bool includeSecrets) const {
  const AppConfig config = snapshot();
  writeConfigJson(root, config, includeSecrets);
  if (!includeSecrets) {
    root["wifi_password"] = "";
    root["netatmo_client_id"] = "";
    root["netatmo_client_secret"] = "";
    root["netatmo_access_token"] = "";
    root["netatmo_refresh_token"] = "";
    root["garmin_client_id"] = "";
    root["garmin_refresh_token"] = "";
    root["market_api_key"] = "";
    root["calendar_ical_url"] = "";
    root["calendar_ical_url_2"] = "";
    root["calendar_ical_url_3"] = "";
    root["calendar_ical_url_4"] = "";
    root["has_wifi_password"] = !config.wifiPassword.isEmpty();
    root["has_netatmo_client_id"] = !config.netatmoClientId.isEmpty();
    root["has_netatmo_client_secret"] =
        !config.netatmoClientSecret.isEmpty();
    root["has_netatmo_access_token"] = !config.netatmoAccessToken.isEmpty();
    root["has_netatmo_refresh_token"] = !config.netatmoRefreshToken.isEmpty();
    root["has_garmin_client_id"] = !config.garminClientId.isEmpty();
    root["has_garmin_refresh_token"] =
        !config.garminRefreshToken.isEmpty();
    root["has_market_api_key"] = !config.marketApiKey.isEmpty();
    root["has_calendar_ical_url"] = !config.calendarIcalUrl.isEmpty();
    root["has_calendar_ical_url_2"] = !config.calendarIcalUrl2.isEmpty();
    root["has_calendar_ical_url_3"] = !config.calendarIcalUrl3.isEmpty();
    root["has_calendar_ical_url_4"] = !config.calendarIcalUrl4.isEmpty();
  }
}

String ConfigStore::browserJson() const {
  const AppConfig config = snapshot();
  JsonStringObject root;
  writeConfigJson(root, config, false);
  root["has_wifi_password"] = !config.wifiPassword.isEmpty();
  root["has_netatmo_client_id"] = !config.netatmoClientId.isEmpty();
  root["has_netatmo_client_secret"] = !config.netatmoClientSecret.isEmpty();
  root["has_netatmo_access_token"] = !config.netatmoAccessToken.isEmpty();
  root["has_netatmo_refresh_token"] = !config.netatmoRefreshToken.isEmpty();
  root["has_garmin_client_id"] = !config.garminClientId.isEmpty();
  root["has_garmin_refresh_token"] = !config.garminRefreshToken.isEmpty();
  root["has_market_api_key"] = !config.marketApiKey.isEmpty();
  root["has_calendar_ical_url"] = !config.calendarIcalUrl.isEmpty();
  root["has_calendar_ical_url_2"] = !config.calendarIcalUrl2.isEmpty();
  root["has_calendar_ical_url_3"] = !config.calendarIcalUrl3.isEmpty();
  root["has_calendar_ical_url_4"] = !config.calendarIcalUrl4.isEmpty();
  return root.finish();
}

void ConfigStore::updateNetatmoTokens(const String& accessToken,
                                      const String& refreshToken) {
  // OAuth rotation happens inside the provider worker while TLS buffers and
  // response objects are still alive. Re-normalising and serialising all 150+
  // settings here used to allocate several vectors and could reboot the ESP32
  // on an otherwise successful token refresh. Tokens already live in their
  // own NVS keys, so update only those fields atomically.
  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool accessSaved = putSecret(preferences_, kNetatmoAccessTokenKey,
                                     accessToken);
  const bool refreshSaved =
      refreshToken.isEmpty() ||
      putSecret(preferences_, kNetatmoRefreshTokenKey, refreshToken);
  if (accessSaved && refreshSaved) {
    config_.netatmoAccessToken = accessToken;
    if (!refreshToken.isEmpty()) {
      config_.netatmoRefreshToken = refreshToken;
    }
  }
  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
}

bool ConfigStore::updateGarminCredentials(const String& domain,
                                          const String& clientId,
                                          const String& refreshToken) {
  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool saved = putGarminCredentials(preferences_, domain, clientId,
                                          refreshToken);
  if (saved) {
    config_.garminDomain = domain;
    config_.garminClientId = clientId;
    config_.garminRefreshToken = refreshToken;
    ++revision_;
  }
  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
  return saved;
}

void ConfigStore::factoryReset() {
  preferences_.clear();
  if (fileSystemReady_) {
    SPIFFS.remove(kConfigPath);
    SPIFFS.remove(kConfigBackupPath);
    SPIFFS.remove(kConfigTemporaryPath);
  }
  loadDefaults();
  config_.apiKey = generateApiKey();
  save(config_);
}

void ConfigStore::normalize(AppConfig& config) {
  const uint16_t previousSchema = config.schemaVersion;
  config.schemaVersion = 28;
  if (previousSchema < 9) {
    if (config.deviceName == "Ulanzi PixelOS") {
      config.deviceName = "Ulanzi PikOS";
    }
    config.climateTemperatureLowIconId = config.climateIconColdId;
    config.climateTemperatureMediumIconId = config.climateIconId;
    config.climateTemperatureHighIconId = config.climateIconHotId;
    config.climateHumidityMediumIconId = config.climateIconId;
    config.netatmoTemperatureLowIconId = config.netatmoIconColdId;
    config.netatmoTemperatureMediumIconId = config.netatmoIconId;
    config.netatmoTemperatureHighIconId = config.netatmoIconHotId;
    config.netatmoHumidityLowIconId = config.netatmoIconSunnyId;
    config.netatmoHumidityMediumIconId = config.netatmoIconId;
    config.netatmoHumidityHighIconId = config.netatmoIconRainId;
  }
  if (previousSchema < 11) {
    // The two climate apps represented the same two measurements. Preserve
    // whichever app the user had selected while collapsing them into one
    // provider-driven widget and one shared set of semantic icons.
    if (config.netatmoEnabled) {
      config.climateProvider = "netatmo";
      config.climateEnabled = true;
      config.climateAnimation = config.netatmoAnimation;
      config.climateTemperatureLowIconId =
          config.netatmoTemperatureLowIconId;
      config.climateTemperatureMediumIconId =
          config.netatmoTemperatureMediumIconId;
      config.climateTemperatureHighIconId =
          config.netatmoTemperatureHighIconId;
      config.climateHumidityLowIconId =
          config.netatmoHumidityLowIconId;
      config.climateHumidityMediumIconId =
          config.netatmoHumidityMediumIconId;
      config.climateHumidityHighIconId =
          config.netatmoHumidityHighIconId;
    } else {
      config.climateProvider = "local";
    }
  }
  if (previousSchema < 12) {
    // The original AWTRIX endpoints report this particular fully charged
    // TC001 as only ~85%. Preserve custom values, but migrate untouched
    // defaults to the observed full point and requested semantic icons.
    config.batteryFullRaw = 640;
    if (config.energyIconLowId == 0) {
      config.energyIconLowId = 60725;
    }
    if (config.energyIconAverageId == 0) {
      config.energyIconAverageId = 60726;
    }
    if (config.energyIconHighId == 0) {
      config.energyIconHighId = 69668;
    }
    config.rssFallbackIconId = 1278;
    String automaticFeeds;
    for (const IconItem& item : parseIconItems(config.rssFeedItems, 6)) {
      const bool legacyBundledLogo =
          item.iconId == 1364 || item.iconId == 4984;
      appendIconItem(automaticFeeds, item.value,
                     legacyBundledLogo ? 0 : item.iconId);
    }
    if (!automaticFeeds.isEmpty()) {
      config.rssFeedItems = automaticFeeds;
    }
  }
  if (previousSchema < 14) {
    // The experimental remote protocols did not integrate with iCUE. Keep
    // upgraded installations on the lightweight authenticated HTTP canvas.
    config.remoteEnabled = false;
    if (config.redditIconId == 0) {
      config.redditIconId = 33701;
    }
  }
  if (previousSchema < 18) {
    // The first battery calibration UI used AWTRIX-derived endpoints that
    // made this unit shut down while still showing roughly 40–50%. Migrate
    // only that untouched low default; user-calibrated values remain intact.
    if (config.batteryEmptyRaw == 475) {
      config.batteryEmptyRaw = 565;
    }
    // Start the common cadence near the former provider defaults so upgrades
    // do not unexpectedly increase network traffic.
  }
  if (previousSchema < 19) {
    // A schema-18 crash reproduction temporarily changed the first bundled
    // r/eesti icon to 33702 on the development unit. Restore only that exact
    // diagnostic value; all other user-selected icons remain untouched.
    String repairedRedditItems;
    for (const IconItem& item : parseIconItems(config.redditItems, 16)) {
      String subreddit = item.value;
      subreddit.toLowerCase();
      const bool bundledEstonia =
          subreddit == "eesti" || subreddit == "r/eesti";
      appendIconItem(repairedRedditItems, item.value,
                     bundledEstonia && item.iconId == 33702 ? 33701
                                                           : item.iconId);
    }
    if (!repairedRedditItems.isEmpty()) {
      config.redditItems = repairedRedditItems;
    }
  }
  if (previousSchema < 21 && config.scrollStepMs == 42) {
    // The original cadence moved one whole column at about 24 fps. A 34 ms
    // step aligns more evenly with the optimized render loop while remaining
    // readable; explicitly customized speeds are left untouched.
    config.scrollStepMs = 34;
  }
  if (previousSchema < 22) {
    // Restore the requested official LaMetric electricity set. Earlier
    // releases accidentally promoted 60724 and could permanently cache a
    // one-frame thumbnail after a transient gallery API failure.
    config.energyIconLowId = 60725;
    config.energyIconAverageId = 60726;
    config.energyIconHighId = 69668;
    SPIFFS.remove("/lm_60725.bin");
    SPIFFS.remove("/lm_60726.bin");
    SPIFFS.remove("/lm_69668.bin");
    // 110 was PikOS's conservative raw FastLED ceiling. Preserve custom
    // ceilings but let an untouched automatic profile use the full range.
    if (config.maximumBrightness == 110) {
      config.maximumBrightness = 255;
    }
  }
  if (previousSchema < 23) {
    // The old minute timer ran invisibly in otherwise animated apps. Start
    // upgraded clocks with the balanced five-round Sync app cadence instead.
    config.syncEveryLoops = 5;
    config.sleepIdleEnabled = false;
    config.sleepIdleMinutes = 30;
    config.sleepDisplayMode = "off";
  }
  if (previousSchema < 28) {
    // Schema 28 gives each weather metric and new local app a meaningful
    // gallery default. Zero meant the generic built-in fallback in all older
    // settings documents, so it is safe to migrate those untouched values.
    if (config.climateWindIconId == 0) config.climateWindIconId = 2298;
    if (config.climateRainIconId == 0) config.climateRainIconId = 72;
    if (config.activityIconId == 0) config.activityIconId = 397;
  }
  if (previousSchema < 15 && config.redditIconId == 0) {
    config.redditIconId = 33701;
  }
  if (previousSchema < 16 && config.redditIconId == 0) {
    config.redditIconId = 33701;
  }
  if (previousSchema < 17) {
    // Schema 15 copied each subreddit's legacy symbol into the new repeatable
    // rows, which kept overriding the requested LaMetric 33701 default. Make
    // the new default visible once; subsequent browser edits remain custom.
    String migratedRedditItems;
    for (const IconItem& item : parseIconItems(config.redditItems, 16)) {
      appendIconItem(migratedRedditItems, item.value, 33701);
    }
    if (!migratedRedditItems.isEmpty()) {
      config.redditItems = migratedRedditItems;
    }
    config.redditIconId = 33701;
  }
  // The advanced remote settings surface was retired; HTTP canvas writes are
  // authenticated by the API key and do not need a listener toggle.
  config.remoteEnabled = false;
  // 6932 was the old static LaMetric calendar default. The built-in icon now
  // draws the actual day of month in the same red/white calendar style, so
  // transparently move existing installations to the dynamic version.
  if (config.calendarIconId == 6932) {
    config.calendarIconId = 0;
  }
  config.climateProvider =
      config.climateProvider == "netatmo" ? "netatmo" : "local";
  config.netatmoEnabled = false;
  config.language = config.language == "et" ? "et" : "en";
  auto clampClockZone = [](String value) {
    value.toLowerCase();
    const String allowed =
        ",off,local,utc,london,lisbon,tallinn,helsinki,berlin,paris,"
        "amsterdam,new_york,toronto,chicago,denver,los_angeles,vancouver,"
        "phoenix,honolulu,mexico_city,sao_paulo,buenos_aires,"
        "johannesburg,cairo,dubai,delhi,mumbai,kathmandu,bangkok,"
        "singapore,hong_kong,shanghai,seoul,tokyo,perth,adelaide,sydney,"
        "brisbane,auckland,";
    return allowed.indexOf("," + value + ",") >= 0 ? value : String("off");
  };
  config.clockTimezone2 = clampClockZone(config.clockTimezone2);
  config.clockTimezone3 = clampClockZone(config.clockTimezone3);
  config.clockTimezone4 = clampClockZone(config.clockTimezone4);
  config.clockTimezoneSeconds =
      constrain(config.clockTimezoneSeconds, 2, 30);
  if (config.clockTimezoneItems.isEmpty()) {
    appendIconItem(config.clockTimezoneItems, "local", config.timeIconId);
    if (config.clockTimezone2 != "off") {
      appendIconItem(config.clockTimezoneItems, config.clockTimezone2,
                     config.timeIconId);
    }
    if (config.clockTimezone3 != "off") {
      appendIconItem(config.clockTimezoneItems, config.clockTimezone3,
                     config.timeIconId);
    }
    if (config.clockTimezone4 != "off") {
      appendIconItem(config.clockTimezoneItems, config.clockTimezone4,
                     config.timeIconId);
    }
  }
  {
    String cleaned;
    std::vector<String> seen;
    for (const IconItem& item : parseIconItems(config.clockTimezoneItems, 8)) {
      const String zone = clampClockZone(item.value);
      if (zone == "off" ||
          std::find(seen.begin(), seen.end(), zone) != seen.end()) {
        continue;
      }
      seen.push_back(zone);
      appendIconItem(cleaned, zone, item.iconId);
    }
    if (cleaned.isEmpty()) {
      appendIconItem(cleaned, "local", config.timeIconId);
    }
    config.clockTimezoneItems = cleaned;
  }
  config.manualBrightness = constrain(config.manualBrightness, 0, 255);
  config.minimumBrightness = constrain(config.minimumBrightness, 0, 255);
  if (previousSchema < 4 && config.minimumBrightness < 10) {
    config.minimumBrightness = 10;
  }
  config.maximumBrightness = constrain(config.maximumBrightness, 1, 255);
  config.ldrDark = constrain(config.ldrDark, 0, 1022);
  config.ldrBright = constrain(config.ldrBright, 1, 1023);
  config.brightnessGamma =
      constrain(config.brightnessGamma, 0.2f, 4.0f);
  config.batteryEmptyRaw =
      constrain(config.batteryEmptyRaw, 350, 700);
  config.batteryFullRaw =
      constrain(config.batteryFullRaw,
                static_cast<int>(config.batteryEmptyRaw) + 30, 800);
  config.batterySaverThreshold =
      constrain(config.batterySaverThreshold, 2, 40);
  config.batterySaverBrightness =
      constrain(config.batterySaverBrightness, 5, 80);
  config.timeAnimation = clampAnimation(config.timeAnimation);
  config.climateAnimation = clampAnimation(config.climateAnimation);
  config.netatmoAnimation = clampAnimation(config.netatmoAnimation);
  config.rssAnimation = clampAnimation(config.rssAnimation);
  config.batteryAnimation = clampAnimation(config.batteryAnimation);
  config.marketAnimation = clampAnimation(config.marketAnimation);
  config.energyAnimation = clampAnimation(config.energyAnimation);
  config.calendarAnimation = clampAnimation(config.calendarAnimation);
  config.travelAnimation = clampAnimation(config.travelAnimation);
  config.redditAnimation = clampAnimation(config.redditAnimation);
  config.activityAnimation = clampAnimation(config.activityAnimation);
  if (!config.activityShowSteps && !config.activityShowDailyDistance &&
      !config.activityShowLatestDistance &&
      !config.activityShowLatestDuration &&
      !config.activityShowLatestCalories) {
    config.activityShowSteps = true;
  }
  config.appDurationSeconds =
      constrain(config.appDurationSeconds, 2, 120);
  config.maximumAppSeconds =
      constrain(config.maximumAppSeconds, config.appDurationSeconds, 180);
  config.scrollStepMs = constrain(config.scrollStepMs, 15, 250);
  config.maximumMessageChars =
      constrain(config.maximumMessageChars, 16, 512);
  if (config.syncEveryLoops != 1 && config.syncEveryLoops != 5 &&
      config.syncEveryLoops != 10) {
    config.syncEveryLoops = 5;
  }
  config.rssHeadlineCount = constrain(config.rssHeadlineCount, 1, 20);
  config.rssRefreshMinutes =
      constrain(config.rssRefreshMinutes, 2, 1440);
  if (config.rssFeedItems.isEmpty()) {
    appendIconItem(config.rssFeedItems, config.rssUrl1, config.rssIconId1);
    appendIconItem(config.rssFeedItems, config.rssUrl2, config.rssIconId2);
  }
  {
    String cleaned;
    for (const IconItem& item : parseIconItems(config.rssFeedItems, 6)) {
      String url = item.value;
      url.trim();
      if (url == "https://www.theverge.com/rss/" ||
          url == "http://www.theverge.com/rss/") {
        url = "https://www.theverge.com/rss/index.xml";
      }
      if (url.startsWith("http://") || url.startsWith("https://")) {
        appendIconItem(cleaned, url, item.iconId);
      }
    }
    config.rssFeedItems = cleaned;
  }
  config.netatmoRefreshMinutes =
      constrain(config.netatmoRefreshMinutes, 2, 1440);
  config.marketRefreshMinutes =
      constrain(config.marketRefreshMinutes, 5, 1440);
  config.energyRefreshMinutes =
      constrain(config.energyRefreshMinutes, 5, 1440);
  config.calendarRefreshMinutes =
      constrain(config.calendarRefreshMinutes, 5, 1440);
  config.calendarMaximumEvents =
      constrain(config.calendarMaximumEvents, 1, 20);
  config.calendarName1.trim();
  config.calendarName2.trim();
  config.calendarName3.trim();
  config.calendarName4.trim();
  if (config.calendarName1.isEmpty()) {
    config.calendarName1 = "Calendar";
  }
  if (config.calendarName1.length() > 24) config.calendarName1.remove(24);
  if (config.calendarName2.length() > 24) config.calendarName2.remove(24);
  if (config.calendarName3.length() > 24) config.calendarName3.remove(24);
  if (config.calendarName4.length() > 24) config.calendarName4.remove(24);
  {
    String cleaned;
    std::vector<String> seen;
    for (const CalendarItem& item :
         parseCalendarItems(config.calendarItems, kMaximumCalendarItems)) {
      String name = item.name;
      String url = item.url;
      name.replace("\t", " ");
      name.replace("\n", " ");
      name.trim();
      url.replace("\t", "");
      url.replace("\n", "");
      url.trim();
      if (name.isEmpty()) name = "Calendar";
      if (name.length() > 24) name.remove(24);
      const bool valid = url.startsWith("http://") ||
                         url.startsWith("https://") || url.startsWith("@");
      if (!valid || std::find(seen.begin(), seen.end(), url) != seen.end()) {
        continue;
      }
      seen.push_back(url);
      appendCalendarItem(cleaned, name, url, item.iconId);
    }
    config.calendarItems = cleaned;
    if (!cleaned.isEmpty()) {
      // Dynamic vault slots supersede the four schema-14 credentials. Do not
      // keep duplicate private URLs alive in every AppConfig snapshot.
      config.calendarIcalUrl = "";
      config.calendarIcalUrl2 = "";
      config.calendarIcalUrl3 = "";
      config.calendarIcalUrl4 = "";
    }
  }
  {
    String cleaned;
    std::vector<String> seen;
    for (const TravelStopItem& item :
         parseTravelStopItems(config.travelStopItems,
                              kMaximumTravelStopItems)) {
      String label = item.label;
      label.replace("\t", " ");
      label.replace("\r", " ");
      label.replace("\n", " ");
      label.trim();

      String stopId;
      for (size_t index = 0; index < item.stopId.length(); ++index) {
        const unsigned char character = item.stopId[index];
        if (isalnum(character) || character == '.' || character == '_' ||
            character == '-') {
          stopId += static_cast<char>(character);
        }
      }
      if (stopId.length() > 12) stopId.remove(12);

      String line;
      for (size_t index = 0; index < item.line.length(); ++index) {
        const unsigned char character = item.line[index];
        if (isalnum(character) || character == '+' || character == '.' ||
            character == '_' || character == '-') {
          line += static_cast<char>(character);
        }
      }
      if (line.length() > 8) line.remove(8);
      if (stopId.isEmpty()) continue;
      if (label.isEmpty()) label = stopId;
      if (label.length() > 24) label.remove(24);

      String key = stopId + "\t" + line;
      key.toLowerCase();
      if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
      seen.push_back(key);
      const uint32_t iconId =
          previousSchema < 28 && item.iconId == 0 ? 11999 : item.iconId;
      appendTravelStopItem(cleaned, label, stopId, line, iconId);
    }
    config.travelStopItems = cleaned;
  }
  config.redditPostCount = constrain(config.redditPostCount, 1, 10);
  config.redditRefreshMinutes =
      constrain(config.redditRefreshMinutes, 5, 1440);
  config.garminDomain.toLowerCase();
  if (!validGarminDomain(config.garminDomain)) {
    config.garminDomain = "garmin.com";
  }
  config.garminDisplayName.replace("\r", "");
  config.garminDisplayName.replace("\n", "");
  config.garminDisplayName.trim();
  if (config.garminDisplayName.length() > 128) {
    config.garminDisplayName.remove(128);
  }
  auto invalidGarminSecret = [](const String& value, size_t maximum) {
    if (value.length() > maximum) return true;
    for (size_t index = 0; index < value.length(); ++index) {
      if (static_cast<uint8_t>(value[index]) < 0x20) return true;
    }
    return false;
  };
  if (invalidGarminSecret(config.garminClientId,
                          kMaximumGarminClientIdBytes) ||
      invalidGarminSecret(config.garminRefreshToken,
                          kMaximumGarminRefreshTokenBytes)) {
    config.garminClientId = "";
    config.garminRefreshToken = "";
    config.activityEnabled = false;
  }
  const String allowedRedditPeriods = ",hour,day,week,month,year,all,";
  config.redditPeriod.toLowerCase();
  if (allowedRedditPeriods.indexOf("," + config.redditPeriod + ",") < 0) {
    config.redditPeriod = "week";
  }
  if (config.redditItems.isEmpty()) {
    size_t start = 0;
    while (start <= config.redditSubreddits.length()) {
      int end = config.redditSubreddits.indexOf(',', start);
      if (end < 0) end = config.redditSubreddits.length();
      String subreddit = config.redditSubreddits.substring(start, end);
      subreddit.trim();
      if (!subreddit.isEmpty()) {
        appendIconItem(config.redditItems, subreddit, config.redditIconId);
      }
      if (static_cast<size_t>(end) >= config.redditSubreddits.length()) break;
      start = static_cast<size_t>(end) + 1;
    }
  }
  {
    String cleaned;
    std::vector<String> seen;
    for (const IconItem& item : parseIconItems(config.redditItems, 16)) {
      String subreddit = item.value;
      subreddit.trim();
      if (subreddit.startsWith("r/")) subreddit.remove(0, 2);
      String safe;
      for (size_t index = 0; index < subreddit.length(); ++index) {
        const char character = subreddit[index];
        if (isalnum(static_cast<unsigned char>(character)) ||
            character == '_' || character == '-') {
          safe += character;
        }
      }
      String comparable = safe;
      comparable.toLowerCase();
      if (safe.isEmpty() ||
          std::find(seen.begin(), seen.end(), comparable) != seen.end()) {
        continue;
      }
      seen.push_back(comparable);
      // LaMetric 33701 is the PikOS default for every subreddit, not merely
      // for newly-added browser rows. This also repairs schema-15 rows that
      // were migrated with a zero ID and therefore kept drawing the old
      // built-in Reddit face.
      appendIconItem(cleaned, safe,
                     item.iconId == 0 ? 33701 : item.iconId);
    }
    config.redditItems = cleaned;
    // redditItems is authoritative from schema 15 onward; avoid retaining a
    // second copy of the same list in each provider snapshot.
    config.redditSubreddits = "";
  }
  config.marketProvider =
      config.marketProvider == "twelve" ? "twelve" : "yahoo";
  const String allowedMarketRanges = ",1d,5d,1mo,6mo,1y,";
  if (allowedMarketRanges.indexOf("," + config.marketChartRange + ",") < 0) {
    config.marketChartRange = "5d";
  }
  if (config.marketItems.isEmpty()) {
    size_t start = 0;
    while (start <= config.marketSymbols.length()) {
      int end = config.marketSymbols.indexOf(',', start);
      if (end < 0) {
        end = config.marketSymbols.length();
      }
      String symbol = config.marketSymbols.substring(start, end);
      symbol.trim();
      if (!symbol.isEmpty()) {
        appendIconItem(
            config.marketItems, symbol,
            strtoul(legacyMappedIcon(config.marketTickerIcons, symbol).c_str(),
                    nullptr, 10));
      }
      if (static_cast<size_t>(end) >= config.marketSymbols.length()) {
        break;
      }
      start = static_cast<size_t>(end) + 1;
    }
  }
  {
    String cleaned;
    std::vector<String> seen;
    for (const IconItem& item : parseIconItems(config.marketItems, 6)) {
      String symbol = item.value;
      symbol.trim();
      symbol.toUpperCase();
      if (symbol.startsWith("$")) {
        symbol.remove(0, 1);
      } else if (symbol.startsWith("€")) {
        symbol.remove(0, String("€").length());
      } else if (symbol.startsWith("£")) {
        symbol.remove(0, String("£").length());
      }
      // Yahoo's bare AIR ticker is a different company. Airbus SE on
      // Euronext Paris is AIR.PA, so keep the friendly alias unambiguous on
      // every save rather than only during a one-time schema migration.
      if (symbol == "AIR") {
        symbol = "AIR.PA";
      }
      if (symbol.isEmpty() ||
          std::find(seen.begin(), seen.end(), symbol) != seen.end()) {
        continue;
      }
      seen.push_back(symbol);
      appendIconItem(cleaned, symbol, item.iconId);
    }
    config.marketItems = cleaned;
  }
  config.energyCountry.toUpperCase();
  if (config.energyCountry != "EE" && config.energyCountry != "FI" &&
      config.energyCountry != "LV" && config.energyCountry != "LT") {
    config.energyCountry = "EE";
  }
  config.energyVatPercent =
      constrain(config.energyVatPercent, 0.0f, 40.0f);
  const String allowedEnergyRanges =
      ",today,next12h,next24h,today_tomorrow,";
  if (allowedEnergyRanges.indexOf("," + config.energyChartRange + ",") < 0) {
    config.energyChartRange = "today";
  }
  config.buttonWakeMinutes =
      constrain(config.buttonWakeMinutes, 1, 120);
  config.buttonShortLeft =
      clampButtonAction(config.buttonShortLeft, "previous");
  config.buttonShortMiddle =
      clampButtonAction(config.buttonShortMiddle, "auto");
  config.buttonShortRight =
      clampButtonAction(config.buttonShortRight, "next");
  config.buttonLongLeft =
      clampButtonAction(config.buttonLongLeft, "refresh");
  config.buttonLongMiddle =
      clampButtonAction(config.buttonLongMiddle, "device_info");
  config.buttonLongRight =
      clampButtonAction(config.buttonLongRight, "none");
  config.sleepIdleMinutes = constrain(config.sleepIdleMinutes, 1, 720);
  config.sleepDisplayMode =
      config.sleepDisplayMode == "clock" ? "clock" : "off";
  config.counterMode = config.counterMode == "up" ? "up" : "down";
  config.counterDurationSeconds =
      constrain(config.counterDurationSeconds, 1UL, 359999UL);
  if (config.alarmItems.length() > 1536) {
    config.alarmItems.remove(1536);
  }
  config.hourlyChimeStart = constrain(config.hourlyChimeStart, 0, 23);
  config.hourlyChimeEnd = constrain(config.hourlyChimeEnd, 0, 23);
  config.newsTone = clampTone(config.newsTone, "news");
  config.climateTone = clampTone(config.climateTone, "soft");
  config.alarmTone = clampTone(config.alarmTone, "alert");
  config.counterTone = clampTone(config.counterTone, "alert");
  config.hourlyTone = clampTone(config.hourlyTone, "hourly");
  config.notificationTone =
      clampTone(config.notificationTone, "double");
  config.buttonTone = clampTone(config.buttonTone, "click");
  config.energyTone = clampTone(config.energyTone, "soft");
  config.calendarTone = clampTone(config.calendarTone, "soft");
  config.disabledBuiltinTones =
      normalizeDisabledBuiltinTones(config.disabledBuiltinTones);
  config.buzzerVolume = constrain(config.buzzerVolume, 0, 100);
  if (previousSchema < 10 && config.buzzerPitchPercent == 100) {
    // A Sennheiser Profile measurement of the TC001 buzzer found the
    // uncompensated RTTTL notes about 25 cents sharp on average.
    config.buzzerPitchPercent = 99;
  }
  if (previousSchema < 10 &&
      config.buzzerHighNoteBoostPercent == 200) {
    // At 5% volume, the top two octaves disappear into the noise without the
    // steeper curve; 250% balanced C5 through C8 within 0.04 dB.
    config.buzzerHighNoteBoostPercent = 250;
  }
  config.buzzerPitchPercent =
      constrain(config.buzzerPitchPercent, 80, 120);
  config.buzzerHighNoteBoostPercent =
      constrain(config.buzzerHighNoteBoostPercent, 100, 300);
  config.customRtttl1.trim();
  config.customRtttl2.trim();
  config.customRtttl3.trim();
  config.customRtttl4.trim();
  config.customRtttl5.trim();
  config.customRtttl6.trim();
  if (config.customRtttl1.length() > 768) {
    config.customRtttl1.remove(768);
  }
  if (config.customRtttl2.length() > 768) {
    config.customRtttl2.remove(768);
  }
  if (config.customRtttl3.length() > 768) {
    config.customRtttl3.remove(768);
  }
  if (config.customRtttl4.length() > 768) {
    config.customRtttl4.remove(768);
  }
  if (config.customRtttl5.length() > 768) {
    config.customRtttl5.remove(768);
  }
  if (config.customRtttl6.length() > 768) {
    config.customRtttl6.remove(768);
  }
  config.remoteTimeoutMs = constrain(config.remoteTimeoutMs, 250, 30000);
  config.deviceName.trim();
  config.timezone.trim();
  config.remoteSourceIp.trim();
  config.marketSymbols.trim();
  config.marketTickerIcons.trim();
  config.redditSubreddits.replace("\n", ",");
  config.redditSubreddits.replace(" ", "");
  if (config.redditSubreddits.length() > 192) {
    config.redditSubreddits.remove(192);
  }
  if (previousSchema < 8 && config.batteryIconHighId == 389 &&
      config.batteryIconId != 389) {
    config.batteryIconHighId = config.batteryIconId;
  }
  if (config.apiKey.length() < 16) {
    config.apiKey = generateApiKey();
  }
}

String ConfigStore::generateApiKey() {
  char output[33];
  for (size_t i = 0; i < 16; ++i) {
    snprintf(output + i * 2, 3, "%02x",
             static_cast<unsigned int>(esp_random() & 0xff));
  }
  output[32] = '\0';
  return String(output);
}

uint32_t parseHtmlColor(const String& value, uint32_t fallback) {
  String normalized = value;
  normalized.trim();
  if (normalized.startsWith("#")) {
    normalized.remove(0, 1);
  }
  if (normalized.length() != 6) {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long parsed = strtoul(normalized.c_str(), &end, 16);
  if (end == nullptr || *end != '\0') {
    return fallback;
  }
  return static_cast<uint32_t>(parsed);
}

String htmlColor(uint32_t color) {
  char output[8];
  snprintf(output, sizeof(output), "#%06lx",
           static_cast<unsigned long>(color & 0xffffff));
  return String(output);
}

std::vector<IconItem> parseIconItems(const String& encoded,
                                    uint8_t maximumItems) {
  std::vector<IconItem> items;
  size_t start = 0;
  while (start <= encoded.length() && items.size() < maximumItems) {
    int end = encoded.indexOf('\n', start);
    if (end < 0) {
      end = encoded.length();
    }
    String record = encoded.substring(start, end);
    record.replace("\r", "");
    const int separator = record.lastIndexOf('\t');
    String value = separator >= 0 ? record.substring(0, separator) : record;
    String id = separator >= 0 ? record.substring(separator + 1) : "0";
    value.trim();
    id.trim();
    if (!value.isEmpty()) {
      items.push_back(
          {value, static_cast<uint32_t>(strtoul(id.c_str(), nullptr, 10))});
    }
    if (static_cast<size_t>(end) >= encoded.length()) {
      break;
    }
    start = static_cast<size_t>(end) + 1;
  }
  return items;
}

std::vector<CalendarItem> parseCalendarItems(const String& encoded,
                                             uint8_t maximumItems) {
  std::vector<CalendarItem> items;
  size_t start = 0;
  while (start <= encoded.length() && items.size() < maximumItems) {
    int end = encoded.indexOf('\n', start);
    if (end < 0) {
      end = encoded.length();
    }
    String record = encoded.substring(start, end);
    record.replace("\r", "");
    const int first = record.indexOf('\t');
    const int last = record.lastIndexOf('\t');
    CalendarItem item;
    if (first >= 0 && last > first) {
      item.name = record.substring(0, first);
      item.url = record.substring(first + 1, last);
      String id = record.substring(last + 1);
      id.trim();
      item.iconId =
          static_cast<uint32_t>(strtoul(id.c_str(), nullptr, 10));
    } else {
      item.url = record;
    }
    item.name.trim();
    item.url.trim();
    if (!item.url.isEmpty()) {
      items.push_back(item);
    }
    if (static_cast<size_t>(end) >= encoded.length()) {
      break;
    }
    start = static_cast<size_t>(end) + 1;
  }
  return items;
}

std::vector<TravelStopItem> parseTravelStopItems(const String& encoded,
                                                 uint8_t maximumItems) {
  std::vector<TravelStopItem> items;
  size_t start = 0;
  while (start <= encoded.length() && items.size() < maximumItems) {
    int end = encoded.indexOf('\n', start);
    if (end < 0) end = encoded.length();
    String record = encoded.substring(start, end);
    record.replace("\r", "");
    const int first = record.indexOf('\t');
    const int second = first < 0 ? -1 : record.indexOf('\t', first + 1);
    const int last = record.lastIndexOf('\t');
    TravelStopItem item;
    if (first >= 0 && second > first && last >= second) {
      item.label = record.substring(0, first);
      item.stopId = record.substring(first + 1, second);
      item.line = last > second ? record.substring(second + 1, last)
                                : String();
      String id = record.substring(last + 1);
      id.trim();
      item.iconId =
          static_cast<uint32_t>(strtoul(id.c_str(), nullptr, 10));
    } else {
      // A bare identifier remains a useful hand-edited recovery format.
      item.stopId = record;
    }
    item.label.trim();
    item.stopId.trim();
    item.line.trim();
    if (!item.stopId.isEmpty()) items.push_back(item);
    if (static_cast<size_t>(end) >= encoded.length()) break;
    start = static_cast<size_t>(end) + 1;
  }
  return items;
}

uint32_t iconForItem(const String& encoded, const String& value) {
  for (const IconItem& item : parseIconItems(encoded)) {
    if (item.value.equalsIgnoreCase(value)) {
      return item.iconId;
    }
  }
  return 0;
}

}  // namespace pixelos
