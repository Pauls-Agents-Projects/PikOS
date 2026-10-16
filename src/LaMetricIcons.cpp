#include "LaMetricIcons.h"

#include "TlsTrust.h"

#include <HTTPClient.h>
#include <SPIFFS.h>
#include <WiFiClientSecure.h>
#include <esp32/rom/miniz.h>

#include "PixelDisplay.h"
#include "TlsHeapGuard.h"
#include "Version.h"

namespace pixelos {
namespace {

constexpr uint32_t kCacheMagic = 0x43494d4c;  // "LMIC" little endian
constexpr uint8_t kCacheVersion = 2;
constexpr uint8_t kMaximumFrames = 32;
// Always reopen the settings listener for a useful window between icons. The
// previous timer was measured from download start, so a slow request could
// finish after the timer and immediately close the listener for the next one.
constexpr uint32_t kRetryMs = 3500;
constexpr uint32_t kFailedIconRetryMs = 60000;

// PNG fallback decoding used two temporary std::vectors. On the fragmented
// heap left by TLS, allocating the ~8–16 KiB inflated image could throw and
// reboot the device (captured in the 0.16.0 crash backtrace). One icon worker
// exists, so fixed BSS workspaces are both safe and deterministic.
constexpr size_t kMaximumCompressedPngBytes = 12288;
constexpr size_t kMaximumInflatedPngBytes = 12288;
constexpr size_t kMaximumGalleryPayloadBytes = 12288;
constexpr size_t kPngWorkspaceBytes =
    sizeof(tinfl_decompressor) + kMaximumCompressedPngBytes +
    kMaximumInflatedPngBytes;

struct PngScratchGuard {
  explicit PngScratchGuard(size_t bytes = 0)
      : data(bytes == 0
                 ? nullptr
                 : static_cast<uint8_t*>(allocateTlsScratch(bytes))) {}
  ~PngScratchGuard() { freeTlsScratch(data); }
  bool allocate(size_t bytes) {
    if (data != nullptr || bytes == 0) return data != nullptr;
    data = static_cast<uint8_t*>(allocateTlsScratch(bytes));
    return data != nullptr;
  }
  uint8_t* data = nullptr;
};

struct CacheHeader {
  uint32_t magic;
  uint32_t iconId;
  uint32_t dominant;
  uint8_t version;
  uint8_t frameCount;
  uint16_t reserved;
};

class ScopedSemaphoreLock {
 public:
  explicit ScopedSemaphoreLock(SemaphoreHandle_t mutex) : mutex_(mutex) {
    if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  ~ScopedSemaphoreLock() {
    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
  }

 private:
  SemaphoreHandle_t mutex_;
};

uint16_t cacheCrc(const uint8_t* bytes, size_t length,
                  uint16_t crc = 0xffff) {
  for (size_t index = 0; index < length; ++index) {
    crc ^= static_cast<uint16_t>(bytes[index]) << 8;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000U) != 0
                ? static_cast<uint16_t>((crc << 1) ^ 0x1021U)
                : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

uint8_t channel(float parsed) {
  if (parsed <= 1.0f) {
    return static_cast<uint8_t>(
        constrain(lroundf(parsed * 255.0f), 0L, 255L));
  }
  return static_cast<uint8_t>(constrain(lroundf(parsed), 0L, 255L));
}

int findToken(const char* json, size_t length, const char* token) {
  if (json == nullptr || token == nullptr) return -1;
  const size_t tokenLength = strlen(token);
  if (tokenLength == 0 || tokenLength > length) return -1;
  for (size_t offset = 0; offset + tokenLength <= length; ++offset) {
    if (memcmp(json + offset, token, tokenLength) == 0) {
      return static_cast<int>(offset);
    }
  }
  return -1;
}

int findCharacter(const char* json, size_t length, char value, int start) {
  if (json == nullptr || start < 0) return -1;
  for (size_t offset = static_cast<size_t>(start); offset < length; ++offset) {
    if (json[offset] == value) return static_cast<int>(offset);
  }
  return -1;
}

int matchingArrayEnd(const char* json, size_t length, int openingBracket) {
  if (openingBracket < 0 ||
      openingBracket >= static_cast<int>(length) ||
      json[openingBracket] != '[') {
    return -1;
  }
  int depth = 0;
  bool inString = false;
  bool escaped = false;
  for (int cursor = openingBracket;
       cursor < static_cast<int>(length); ++cursor) {
    const char current = json[cursor];
    if (inString) {
      if (escaped) {
        escaped = false;
      } else if (current == '\\') {
        escaped = true;
      } else if (current == '"') {
        inString = false;
      }
      continue;
    }
    if (current == '"') {
      inString = true;
    } else if (current == '[') {
      ++depth;
    } else if (current == ']' && --depth == 0) {
      return cursor;
    }
  }
  return -1;
}

struct WantedIcon {
  uint32_t id;
  String sourceUrl;
};

uint32_t faviconHash(const String& url) {
  uint32_t hash = 2166136261UL;
  // Version the derived ID so an old generic/corrupt favicon cache cannot
  // survive after the user switches a feed back to automatic mode.
  hash ^= 2;
  hash *= 16777619UL;
  for (size_t index = 0; index < url.length(); ++index) {
    hash ^= static_cast<uint8_t>(url[index]);
    hash *= 16777619UL;
  }
  return 0x80000000UL | (hash & 0x7fffffffUL);
}

String urlEncode(const String& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 2);
  for (size_t index = 0; index < value.length(); ++index) {
    const uint8_t c = static_cast<uint8_t>(value[index]);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += kHex[c >> 4];
      encoded += kHex[c & 0x0f];
    }
  }
  return encoded;
}

std::vector<WantedIcon> wantedIcons(const AppConfig& config) {
  std::vector<WantedIcon> wanted;
  auto addId = [&wanted](uint32_t id) {
    if (id != 0) wanted.push_back({id, ""});
  };
  auto addGallery = [&wanted](const String& list) {
    for (const IconItem& item : parseIconItems(list)) {
      if (item.iconId != 0) wanted.push_back({item.iconId, ""});
    }
  };

  // Queue only artwork that an enabled app can actually render. The merged
  // Climate app deliberately uses the shared climate IDs for both its local
  // and Netatmo providers; old netatmo* icon fields remain migration inputs,
  // not assets that consume sync time and SPIFFS forever.
  if (config.energyEnabled) {
    addId(config.energyIconLowId);
    addId(config.energyIconAverageId);
    addId(config.energyIconHighId);
  }
  if (config.climateEnabled) {
    // Temperature is a generated five-level mercury column, so the legacy
    // three-image temperature set must not consume cache space or Sync time.
    addId(config.climateHumidityLowIconId);
    addId(config.climateHumidityMediumIconId);
    addId(config.climateHumidityHighIconId);
    addId(config.climateWindIconId);
    addId(config.climateRainIconId);
  }
  if (config.batteryEnabled || config.batteryAutoLowEnabled) {
    addId(config.batteryIconHighId);
    addId(config.batteryIconMediumId);
    addId(config.batteryIconLowId);
  }
  if (config.counterEnabled) addId(config.counterIconId);
  if (config.activityEnabled) addId(config.activityIconId);
  if (config.timeEnabled) addGallery(config.clockTimezoneItems);
  if (config.marketEnabled) addGallery(config.marketItems);
  if (config.redditEnabled) {
    addId(config.redditIconId);
    addGallery(config.redditItems);
  }
  if (config.rssEnabled) {
    addId(config.rssFallbackIconId);
    for (const IconItem& item : parseIconItems(config.rssFeedItems, 6)) {
      if (item.iconId != 0) {
        wanted.push_back({item.iconId, ""});
      } else if (!item.value.isEmpty()) {
        wanted.push_back({faviconHash(item.value), item.value});
      }
    }
  }
  if (config.calendarEnabled) {
    addId(config.calendarIconId);
    for (const CalendarItem& item :
         parseCalendarItems(config.calendarItems, 16)) {
      if (item.iconId != 0) wanted.push_back({item.iconId, ""});
    }
  }
  if (config.travelEnabled) {
    addId(11999);
    for (const TravelStopItem& item :
         parseTravelStopItems(config.travelStopItems)) {
      addId(item.iconId);
    }
  }
  return wanted;
}

uint32_t bigEndian32(const uint8_t* value) {
  return (static_cast<uint32_t>(value[0]) << 24) |
         (static_cast<uint32_t>(value[1]) << 16) |
         (static_cast<uint32_t>(value[2]) << 8) | value[3];
}

uint8_t paethPredictor(uint8_t left, uint8_t up, uint8_t upperLeft) {
  const int estimate = static_cast<int>(left) + up - upperLeft;
  const int leftDistance = abs(estimate - left);
  const int upDistance = abs(estimate - up);
  const int cornerDistance = abs(estimate - upperLeft);
  if (leftDistance <= upDistance && leftDistance <= cornerDistance) {
    return left;
  }
  return upDistance <= cornerDistance ? up : upperLeft;
}

bool decodeCompactPng(const String& payload, CRGB output[64]) {
  // The HTTP request has ended before this function runs, so the private TLS
  // arena is idle. Borrow it for deterministic PNG scratch, then return it
  // before the next provider handshake. This avoids both general-heap
  // fragmentation and permanent inflater/scratch reservations.
  PngScratchGuard workspace(kPngWorkspaceBytes);
  if (workspace.data == nullptr) return false;
  tinfl_decompressor* const inflater =
      reinterpret_cast<tinfl_decompressor*>(workspace.data);
  uint8_t* const compressedWorkspace =
      workspace.data + sizeof(tinfl_decompressor);
  uint8_t* const inflatedWorkspace =
      compressedWorkspace + kMaximumCompressedPngBytes;
  static constexpr uint8_t kSignature[] =
      {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  const uint8_t* bytes =
      reinterpret_cast<const uint8_t*>(payload.c_str());
  if (payload.length() < 33 || memcmp(bytes, kSignature, 8) != 0) {
    return false;
  }

  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t bitDepth = 0;
  uint8_t colorType = 255;
  uint8_t interlace = 1;
  uint8_t palette[256][3]{};
  uint8_t paletteAlpha[256];
  memset(paletteAlpha, 255, sizeof(paletteAlpha));
  uint16_t paletteSize = 0;
  size_t compressedSize = 0;

  size_t cursor = 8;
  while (cursor + 12 <= payload.length()) {
    const uint32_t length = bigEndian32(bytes + cursor);
    if (length > payload.length() - cursor - 12) {
      return false;
    }
    const uint8_t* type = bytes + cursor + 4;
    const uint8_t* data = bytes + cursor + 8;
    if (memcmp(type, "IHDR", 4) == 0 && length == 13) {
      width = bigEndian32(data);
      height = bigEndian32(data + 4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (memcmp(type, "PLTE", 4) == 0) {
      paletteSize = min<uint32_t>(length / 3, 256);
      for (uint16_t index = 0; index < paletteSize; ++index) {
        memcpy(palette[index], data + index * 3, 3);
      }
    } else if (memcmp(type, "tRNS", 4) == 0) {
      memcpy(paletteAlpha, data, min<uint32_t>(length, 256));
    } else if (memcmp(type, "IDAT", 4) == 0) {
      if (length > kMaximumCompressedPngBytes - compressedSize) return false;
      memcpy(compressedWorkspace + compressedSize, data, length);
      compressedSize += length;
    } else if (memcmp(type, "IEND", 4) == 0) {
      break;
    }
    cursor += length + 12;
  }

  uint8_t channels = 0;
  if (colorType == 0) channels = 1;
  if (colorType == 2) channels = 3;
  if (colorType == 3) channels = 1;
  if (colorType == 4) channels = 2;
  if (colorType == 6) channels = 4;
  if (width == 0 || height == 0 || width > 64 || height > 64 ||
      bitDepth != 8 || channels == 0 || interlace != 0 ||
      compressedSize == 0 || (colorType == 3 && paletteSize == 0)) {
    return false;
  }

  const size_t rowBytes = width * channels;
  const size_t inflatedSize = height * (rowBytes + 1);
  if (inflatedSize > kMaximumInflatedPngBytes) return false;
  tinfl_init(inflater);
  size_t inputSize = compressedSize;
  size_t outputSize = inflatedSize;
  const tinfl_status inflateStatus = tinfl_decompress(
      inflater, compressedWorkspace, &inputSize, inflatedWorkspace,
      inflatedWorkspace, &outputSize,
      TINFL_FLAG_PARSE_ZLIB_HEADER |
          TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  if (inflateStatus != TINFL_STATUS_DONE || outputSize != inflatedSize) {
    return false;
  }

  uint32_t red[64]{};
  uint32_t green[64]{};
  uint32_t blue[64]{};
  uint16_t count[64]{};
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* row = inflatedWorkspace + y * (rowBytes + 1) + 1;
    const uint8_t filter = row[-1];
    const uint8_t* previous =
        y == 0 ? nullptr
               : inflatedWorkspace + (y - 1) * (rowBytes + 1) + 1;
    if (filter > 4) {
      return false;
    }
    for (size_t byte = 0; byte < rowBytes; ++byte) {
      const uint8_t left = byte >= channels ? row[byte - channels] : 0;
      const uint8_t up = previous == nullptr ? 0 : previous[byte];
      const uint8_t upperLeft =
          previous == nullptr || byte < channels
              ? 0
              : previous[byte - channels];
      if (filter == 1) row[byte] += left;
      if (filter == 2) row[byte] += up;
      if (filter == 3) row[byte] += (left + up) / 2;
      if (filter == 4) row[byte] += paethPredictor(left, up, upperLeft);
    }

    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* pixel = row + x * channels;
      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;
      uint8_t alpha = 255;
      if (colorType == 0 || colorType == 4) {
        r = g = b = pixel[0];
        if (colorType == 4) alpha = pixel[1];
      } else if (colorType == 2 || colorType == 6) {
        r = pixel[0];
        g = pixel[1];
        b = pixel[2];
        if (colorType == 6) alpha = pixel[3];
      } else {
        const uint8_t paletteIndex = pixel[0];
        if (paletteIndex >= paletteSize) continue;
        r = palette[paletteIndex][0];
        g = palette[paletteIndex][1];
        b = palette[paletteIndex][2];
        alpha = paletteAlpha[paletteIndex];
      }
      if (alpha < 16) {
        continue;
      }
      const uint8_t destination =
          min<uint8_t>(7, y * 8 / height) * 8 +
          min<uint8_t>(7, x * 8 / width);
      red[destination] += static_cast<uint16_t>(r) * alpha / 255;
      green[destination] += static_cast<uint16_t>(g) * alpha / 255;
      blue[destination] += static_cast<uint16_t>(b) * alpha / 255;
      ++count[destination];
    }
  }
  for (uint8_t index = 0; index < 64; ++index) {
    output[index] =
        count[index] == 0
            ? CRGB::Black
            : CRGB(red[index] / count[index], green[index] / count[index],
                   blue[index] / count[index]);
  }
  return true;
}

}  // namespace

String LaMetricIcons::cachePath(uint32_t iconId) {
  return "/lm_" + String(iconId) + ".bin";
}

bool LaMetricIcons::begin(const AppConfig& config) {
  mutex_ = xSemaphoreCreateMutex();
  fsMutex_ = xSemaphoreCreateMutex();
  icons_.reserve(kMaximumResidentIcons);
  // ConfigStore mounts (and, only when necessary, formats) SPIFFS first.
  // Never format here: this partition now also holds PikOS settings.
  fileSystemReady_ = SPIFFS.begin(false);
  if (!fileSystemReady_) {
    setLastError("SPIFFS unavailable");
    return false;
  }

  reconcileCache(config);
  Serial.printf("[icons] %u cached LaMetric icon(s) ready\n",
                static_cast<unsigned>(cachedCount()));
  return true;
}

void LaMetricIcons::reconcileCache(const AppConfig& config) {
  if (!fileSystemReady_) return;
  const std::vector<WantedIcon> wanted = wantedIcons(config);
  ScopedSemaphoreLock fsLock(fsMutex_);
  auto validCache = [](const String& path, uint32_t expectedId,
                       size_t& bytes) {
    File cached = SPIFFS.open(path, FILE_READ);
    if (!cached) return false;
    bytes = cached.size();
    CacheHeader header{};
    bool valid =
        cached.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) ==
            sizeof(header) &&
        header.magic == kCacheMagic && header.version == kCacheVersion &&
        header.iconId == expectedId && header.frameCount > 0 &&
        header.frameCount <= kMaximumFrames &&
        bytes == sizeof(CacheHeader) +
                     static_cast<size_t>(header.frameCount) *
                         sizeof(IconFrame);
    uint16_t crc = 0xffff;
    IconFrame frame;
    for (uint8_t index = 0; valid && index < header.frameCount; ++index) {
      valid = cached.read(reinterpret_cast<uint8_t*>(&frame), sizeof(frame)) ==
              sizeof(frame);
      if (valid) {
        crc = cacheCrc(reinterpret_cast<const uint8_t*>(&frame),
                       sizeof(frame), crc);
      }
    }
    cached.close();
    return valid && crc == header.reserved;
  };
  File root = SPIFFS.open("/");
  if (!root || !root.isDirectory()) return;
  size_t keptCount = 0;
  size_t keptBytes = 0;
  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (!name.startsWith("/")) name = "/" + name;
    file.close();
    if (name.startsWith("/lm_") && name.endsWith(".tmp")) {
      SPIFFS.remove(name);
    } else if (name.startsWith("/lm_") && name.endsWith(".bin")) {
      const uint32_t id =
          strtoul(name.substring(4, name.length() - 4).c_str(), nullptr, 10);
      bool keep = false;
      for (const WantedIcon& item : wanted) {
        if (item.id == id) {
          keep = true;
          break;
        }
      }
      size_t fileBytes = 0;
      if (keep && !validCache(name, id, fileBytes)) {
        keep = false;
        Serial.printf("[icons] rejected invalid cache %s\n", name.c_str());
      }
      if (!keep) {
        SPIFFS.remove(name);
        Serial.printf("[icons] pruned unused cache %s\n", name.c_str());
      } else {
        ++keptCount;
        keptBytes += fileBytes;
      }
    }
    file = root.openNextFile();
  }
  root.close();
  cachedFileCount_ = keptCount;
  cachedFileBytes_ = keptBytes;
}

bool LaMetricIcons::hasIconLocked(uint32_t iconId) const {
  for (const CachedIcon& icon : icons_) {
    if (icon.id == iconId && !icon.frames.empty()) {
      return true;
    }
  }
  return false;
}

bool LaMetricIcons::ready(uint32_t iconId) const {
  if (iconId == 0) {
    return false;
  }
  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
  const bool resident = hasIconLocked(iconId);
  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
  if (resident) return true;
  // A corrupt cache or an incomplete gallery response is quarantined until
  // the icon worker publishes a newly validated file. Without this guard the
  // 64 fps display path can reopen/remove the same bad path every frame.
  if (failedIconId_ == iconId) return false;
  ScopedSemaphoreLock fsLock(fsMutex_);
  return SPIFFS.exists(cachePath(iconId));
}

size_t LaMetricIcons::cachedCount() const {
  ScopedSemaphoreLock fsLock(fsMutex_);
  const size_t count = cachedFileCount_;
  return count;
}

size_t LaMetricIcons::cachedBytes() const {
  if (!fileSystemReady_) return 0;
  ScopedSemaphoreLock fsLock(fsMutex_);
  return cachedFileBytes_;
}

String LaMetricIcons::lastError() const {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  const String error = lastError_;
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  return error;
}

void LaMetricIcons::setLastError(const String& error) {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  lastError_ = error;
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
}

void LaMetricIcons::addResidentLocked(CachedIcon&& icon) {
  // Cache files retain every original frame. Never resample animation time:
  // doing so changed the cadence and erased detail from short gallery loops.
  for (auto existing = icons_.begin(); existing != icons_.end(); ++existing) {
    if (existing->id == icon.id) {
      icons_.erase(existing);
      break;
    }
  }
  while (icons_.size() >= kMaximumResidentIcons) {
    icons_.erase(icons_.begin());
  }
  icons_.push_back(std::move(icon));
}

void LaMetricIcons::tick(bool wifiConnected, const AppConfig& config,
                         uint32_t nowMs) {
  if (!fileSystemReady_ || !wifiConnected) {
    syncScanPending_ = false;
    return;
  }
  if (downloading_ ||
      (!syncScanPending_ && nowMs - lastAttemptMs_ < kRetryMs) ||
      (lastFailureMs_ != 0 &&
       nowMs - lastFailureMs_ < kFailedIconRetryMs)) {
    if (syncScanPending_ && lastFailureMs_ != 0 &&
        nowMs - lastFailureMs_ < kFailedIconRetryMs) {
      syncScanPending_ = false;
    }
    return;
  }
  // Icon discovery is optional background work and currently parses several
  // user lists into short-lived vectors. After a TLS-heavy sync the general
  // heap can be fragmented even though cached icons render normally. Never
  // let a missing allocation in this opportunistic scanner terminate PikOS;
  // retry automatically once the allocator has recovered.
  if (ESP.getFreeHeap() < 20000 || ESP.getMaxAllocHeap() < 8192) {
    lastAttemptMs_ = nowMs;
    syncScanPending_ = false;
    return;
  }

  WantedIcon missing{0, ""};
  for (const WantedIcon& icon : wantedIcons(config)) {
    const bool coolingDown =
        icon.id == failedIconId_ &&
        nowMs - failedIconAtMs_ < kFailedIconRetryMs;
    if (icon.id != 0 && !ready(icon.id) && !coolingDown) {
      missing = icon;
      break;
    }
  }
  if (missing.id == 0) {
    // Rate-limit the no-work path too. Previously this rebuilt every icon
    // list on virtually every main-loop pass once the retry window elapsed.
    lastAttemptMs_ = nowMs;
    syncScanPending_ = false;
    return;
  }

  lastAttemptMs_ = nowMs;
  syncScanPending_ = false;
  pendingIconId_ = missing.id;
  pendingSourceUrl_ = missing.sourceUrl;
  downloading_ = true;
  if (xTaskCreatePinnedToCore(downloadTask, "lametric-icon",
                              kWorkerStackBytes, this, 0, &workerTask_, 0) !=
      pdPASS) {
    workerTask_ = nullptr;
    downloading_ = false;
    setLastError("Could not start icon worker");
  }
}

void LaMetricIcons::downloadTask(void* parameter) {
  LaMetricIcons* owner = static_cast<LaMetricIcons*>(parameter);
  const uint32_t iconId = owner->pendingIconId_;
  const String sourceUrl = owner->pendingSourceUrl_;
  Serial.printf("[icons] downloading %s icon %lu\n",
                sourceUrl.isEmpty() ? "LaMetric" : "favicon",
                static_cast<unsigned long>(iconId));
  bool ok = false;
  {
    ScopedTlsHeapLease tlsHeap;
    if (sourceUrl.isEmpty()) {
      ok = owner->download(iconId);
      if (!ok) {
        // Keep a previously validated animation on a transient API failure.
        // A gallery thumbnail is deliberately not accepted: doing so turns
        // an animated icon into a permanent one-frame cache entry.
        if (owner->ready(iconId)) {
          ok = true;
        }
      }
    } else {
      ok = owner->downloadFavicon(iconId, sourceUrl);
    }
  }
  if (ok) {
    owner->lastFailureMs_ = 0;
    if (owner->failedIconId_ == iconId) {
      owner->failedIconId_ = 0;
      owner->failedIconAtMs_ = 0;
    }
  } else {
    owner->lastFailureMs_ = millis();
    owner->failedIconId_ = iconId;
    owner->failedIconAtMs_ = millis();
    const String reason = owner->lastError();
    owner->setLastError("Icon " + String(iconId) + ": " + reason);
  }
  Serial.printf("[icons] icon %lu %s; worker stack reserve: %u\n",
                static_cast<unsigned long>(iconId),
                ok ? "ready" : owner->lastError().c_str(),
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
  owner->pendingSourceUrl_ = "";
  owner->pendingIconId_ = 0;
  owner->lastAttemptMs_ = millis();
  owner->networkCooldownUntilMs_ = millis() + 1000;
  // A Sync app drains all missing assets, one bounded TLS job at a time.
  owner->syncScanPending_ = true;
  owner->workerTask_ = nullptr;
  owner->downloading_ = false;
  vTaskDelete(nullptr);
}

bool LaMetricIcons::download(uint32_t iconId) {
  const String url =
      "https://developer.lametric.com/api/v1/dev/preloadicondata?icon_id=" +
      String(iconId);
  PngScratchGuard payloadScratch;
  size_t payloadLength = 0;
  int status = 0;
  int expectedLength = -1;
  // The gallery occasionally closes an otherwise successful TLS response a
  // few hundred bytes early. Accept only a complete icons array and force a
  // close-delimited HTTP/1.0 response so the ESP32 never reuses a half-closed
  // connection. Optional assets get one bounded attempt per central Sync;
  // the next configured Sync is the retry, keeping one icon from extending
  // the foreground Sync app for minutes.
  constexpr uint8_t kAttempts = 1;
  for (uint8_t attempt = 0; attempt < kAttempts; ++attempt) {
    WiFiClientSecure client;
    configureTrustedTlsClient(client);
    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(8000);
    http.setReuse(false);
    http.useHTTP10(true);
    if (!http.begin(client, url)) {
      status = HTTPC_ERROR_CONNECTION_REFUSED;
    } else {
      http.setUserAgent(kUserAgent);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      http.addHeader("Accept", "application/json");
      http.addHeader("Accept-Encoding", "identity");
      http.addHeader("Connection", "close");
      status = http.POST("");
      if (status == HTTP_CODE_OK) {
        expectedLength = http.getSize();
        if (expectedLength <= 0 ||
            expectedLength > static_cast<int>(kMaximumGalleryPayloadBytes)) {
          status = HTTPC_ERROR_TOO_LESS_RAM;
        } else if (!payloadScratch.allocate(
                       static_cast<size_t>(expectedLength) + 1U)) {
          status = HTTPC_ERROR_TOO_LESS_RAM;
        } else {
          WiFiClient* stream = http.getStreamPtr();
          payloadLength = stream == nullptr
                              ? 0
                              : stream->readBytes(
                                    payloadScratch.data,
                                    static_cast<size_t>(expectedLength));
          payloadScratch.data[payloadLength] = '\0';
        }
      }
      http.end();
    }

    const char* payload =
        reinterpret_cast<const char*>(payloadScratch.data);
    int attemptIconsKey =
        findToken(payload, payloadLength, "\\\"icons\\\"");
    if (attemptIconsKey < 0) {
      attemptIconsKey = findToken(payload, payloadLength, "\"icons\"");
    }
    const int attemptIconsStart =
        attemptIconsKey < 0
            ? -1
            : findCharacter(payload, payloadLength, '[', attemptIconsKey);
    const bool completeEnvelope =
        matchingArrayEnd(payload, payloadLength, attemptIconsStart) >
        attemptIconsStart;
    const bool completeLength =
        expectedLength < 0 ||
        static_cast<int>(payloadLength) == expectedLength;
    if (status == HTTP_CODE_OK && completeEnvelope && completeLength) break;
    if (attempt + 1 < kAttempts) {
      payloadLength = 0;
      vTaskDelay(pdMS_TO_TICKS(350 + attempt * 250));
    }
  }
  if (status != HTTP_CODE_OK) {
    setLastError("LaMetric HTTP " + String(status));
    return false;
  }

  CachedIcon icon;
  icon.id = iconId;

  const char* payload =
      reinterpret_cast<const char*>(payloadScratch.data);
  int iconsKey = findToken(payload, payloadLength, "\\\"icons\\\"");
  if (iconsKey < 0) {
    iconsKey = findToken(payload, payloadLength, "\"icons\"");
  }
  const int iconsStart =
      iconsKey < 0 ? -1
                   : findCharacter(payload, payloadLength, '[', iconsKey);
  const int iconsEnd =
      matchingArrayEnd(payload, payloadLength, iconsStart);
  int delaysKey = findToken(payload, payloadLength, "\\\"delays\\\"");
  if (delaysKey < 0) {
    delaysKey = findToken(payload, payloadLength, "\"delays\"");
  }
  if (iconsStart < 0 || iconsEnd <= iconsStart) {
    String detail = "Invalid LaMetric envelope (" + String(payloadLength);
    if (expectedLength >= 0) detail += "/" + String(expectedLength);
    detail += " bytes)";
    setLastError(detail);
    return false;
  }

  IconFrame frame;
  float components[4]{};
  uint8_t componentIndex = 0;
  uint8_t pixelIndex = 0;
  int cursor = iconsStart + 1;
  while (cursor < iconsEnd && icon.frames.size() < kMaximumFrames) {
    const char current = payload[cursor];
    if (!isDigit(current) && current != '-' && current != '.') {
      ++cursor;
      continue;
    }
    char* end = nullptr;
    const float value = strtof(payload + cursor, &end);
    if (end == payload + cursor) {
      ++cursor;
      continue;
    }
    cursor = end - payload;
    components[componentIndex++] = value;
    if (componentIndex < 4) {
      continue;
    }
    componentIndex = 0;
    frame.pixels[pixelIndex++] =
        components[3] > 0.01f
            ? CRGB(channel(components[0]), channel(components[1]),
                   channel(components[2]))
            : CRGB::Black;
    if (pixelIndex == 64) {
      icon.frames.push_back(frame);
      frame = IconFrame{};
      pixelIndex = 0;
    }
  }
  if (icon.frames.empty() || pixelIndex != 0 || componentIndex != 0) {
    setLastError("Unsupported LaMetric icon");
    return false;
  }

  const int delaysStart =
      delaysKey < 0
          ? -1
          : findCharacter(payload, payloadLength, '[', delaysKey);
  if (delaysStart >= 0) {
    cursor = delaysStart + 1;
    size_t frameIndex = 0;
    while (cursor < static_cast<int>(payloadLength) &&
           frameIndex < icon.frames.size()) {
      const char current = payload[cursor];
      if (!isDigit(current)) {
        if (current == ']') {
          break;
        }
        ++cursor;
        continue;
      }
      char* end = nullptr;
      const long delay = strtol(payload + cursor, &end, 10);
      if (end == payload + cursor) {
        ++cursor;
        continue;
      }
      cursor = end - payload;
      icon.frames[frameIndex++].delayMs =
          static_cast<uint16_t>(constrain(delay, 40L, 5000L));
    }
  }
  icon.dominant = calculateDominant(icon.frames);
  if (!saveCached(icon)) {
    setLastError("Could not cache LaMetric icon");
    return false;
  }

  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  addResidentLocked(std::move(icon));
  lastError_ = "";
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  return true;
}

uint32_t LaMetricIcons::faviconId(const String& feedUrl) const {
  return feedUrl.isEmpty() ? 0 : faviconHash(feedUrl);
}

bool LaMetricIcons::downloadFavicon(uint32_t iconId,
                                    const String& feedUrl) {
  const String encoded = urlEncode(feedUrl);
  const String primary =
      "https://www.google.com/s2/favicons?domain_url=" + encoded +
      "&sz=16";
  if (downloadPngIcon(iconId, primary, "Favicon", 1)) {
    return true;
  }
  const String fallback =
      "https://t2.gstatic.com/faviconV2?client=SOCIAL&type=FAVICON&"
      "fallback_opts=TYPE,SIZE,URL&url=" +
      encoded + "&size=16";
  return downloadPngIcon(iconId, fallback, "Favicon fallback", 1);
}

bool LaMetricIcons::downloadPngIcon(uint32_t iconId, const String& url,
                                    const char* description,
                                    uint8_t attempts) {
  String payload;
  int lastStatus = 0;
  for (uint8_t attempt = 0; attempt < max<uint8_t>(attempts, 1); ++attempt) {
    WiFiClientSecure client;
    configureTrustedTlsClient(client);
    HTTPClient http;
    http.setConnectTimeout(6000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.useHTTP10(true);
    http.setUserAgent(kUserAgent);
    if (!http.begin(client, url)) {
      lastStatus = HTTPC_ERROR_CONNECTION_REFUSED;
    } else {
      http.addHeader("Accept", "image/png");
      http.addHeader("Accept-Encoding", "identity");
      http.addHeader("Connection", "close");
      lastStatus = http.GET();
      if (lastStatus == HTTP_CODE_OK) {
        payload = http.getString();
      }
      http.end();
    }
    if (lastStatus == HTTP_CODE_OK && !payload.isEmpty()) {
      break;
    }
    if (attempt + 1 < attempts) {
      vTaskDelay(pdMS_TO_TICKS(350 + attempt * 250));
    }
  }
  if (lastStatus != HTTP_CODE_OK || payload.isEmpty()) {
    const String detail =
        lastStatus < 0 ? HTTPClient::errorToString(lastStatus)
                       : String(lastStatus);
    setLastError(String(description) + " HTTP " + detail);
    return false;
  }
  if (payload.length() < 32 || payload.length() > 65535) {
    setLastError(String("Invalid ") + description + " size");
    return false;
  }

  CachedIcon icon;
  icon.id = iconId;
  IconFrame frame;
  if (!decodeCompactPng(payload, frame.pixels)) {
    setLastError(String("Unsupported ") + description + " PNG");
    return false;
  }
  icon.frames.push_back(frame);
  icon.dominant = calculateDominant(icon.frames);
  if (!saveCached(icon)) {
    setLastError(String("Could not cache ") + description);
    return false;
  }
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  addResidentLocked(std::move(icon));
  lastError_ = "";
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  return true;
}

bool LaMetricIcons::saveCached(const CachedIcon& icon) {
  ScopedSemaphoreLock fsLock(fsMutex_);
  const String path = cachePath(icon.id);
  const String temporaryPath = path + ".tmp";
  const bool alreadyStored = SPIFFS.exists(path);
  size_t previousBytes = 0;
  if (alreadyStored) {
    File previous = SPIFFS.open(path, FILE_READ);
    if (previous) {
      previousBytes = previous.size();
      previous.close();
    }
  }
  if (SPIFFS.exists(temporaryPath)) SPIFFS.remove(temporaryPath);
  File file = SPIFFS.open(temporaryPath, FILE_WRITE);
  if (!file) {
    return false;
  }
  uint16_t crc = 0xffff;
  for (const IconFrame& frame : icon.frames) {
    crc = cacheCrc(reinterpret_cast<const uint8_t*>(&frame), sizeof(frame),
                   crc);
  }
  const CacheHeader header{kCacheMagic,
                           icon.id,
                           icon.dominant,
                           kCacheVersion,
                           static_cast<uint8_t>(icon.frames.size()),
                           crc};
  bool ok = file.write(reinterpret_cast<const uint8_t*>(&header),
                       sizeof(header)) == sizeof(header);
  for (const IconFrame& frame : icon.frames) {
    ok = ok &&
         file.write(reinterpret_cast<const uint8_t*>(&frame),
                    sizeof(frame)) == sizeof(frame);
  }
  file.close();
  if (!ok) {
    SPIFFS.remove(temporaryPath);
    return false;
  }
  // Publish only a completely written, CRC-protected file. Readers are
  // excluded by fsMutex_, so they can never classify an in-progress cache as
  // corrupt and delete it. A reset can leave an expendable .tmp or no cache,
  // but never a partially written final path.
  if (alreadyStored && !SPIFFS.remove(path)) {
    SPIFFS.remove(temporaryPath);
    return false;
  }
  if (!SPIFFS.rename(temporaryPath, path)) {
    SPIFFS.remove(temporaryPath);
    return false;
  }
  File published = SPIFFS.open(path, FILE_READ);
  const size_t publishedBytes = published ? published.size() : 0;
  if (published) published.close();
  if (!alreadyStored) {
    ++cachedFileCount_;
  }
  cachedFileBytes_ =
      cachedFileBytes_ >= previousBytes
          ? cachedFileBytes_ - previousBytes + publishedBytes
          : publishedBytes;
  return true;
}

bool LaMetricIcons::loadCached(uint32_t iconId) {
  if (failedIconId_ == iconId) return false;
  CachedIcon icon;
  {
    ScopedSemaphoreLock fsLock(fsMutex_);
    const String path = cachePath(iconId);
    if (!SPIFFS.exists(path)) return false;
    File file = SPIFFS.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      failedIconId_ = iconId;
      failedIconAtMs_ = 0;
      syncScanPending_ = true;
      return false;
    }
    const size_t invalidBytes = file.size();
    auto discardInvalid = [&]() {
      file.close();
      const bool removed = SPIFFS.exists(path) && SPIFFS.remove(path);
      if (removed && cachedFileCount_ > 0) {
        --cachedFileCount_;
        cachedFileBytes_ =
            cachedFileBytes_ >= invalidBytes
                ? cachedFileBytes_ - invalidBytes
                : 0;
      }
      failedIconId_ = iconId;
      failedIconAtMs_ = 0;
      syncScanPending_ = true;
    };
    CacheHeader header{};
    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) !=
            sizeof(header) ||
        header.magic != kCacheMagic || header.version != kCacheVersion ||
        header.iconId != iconId || header.frameCount == 0 ||
        header.frameCount > kMaximumFrames) {
      discardInvalid();
      return false;
    }

    icon.id = iconId;
    icon.dominant = header.dominant;
    const uint8_t residentFrames = header.frameCount;
    icon.frames.resize(residentFrames);
    uint32_t delays[kMaximumResidentFrames]{};
    uint16_t crc = 0xffff;
    for (uint8_t sourceIndex = 0; sourceIndex < header.frameCount;
         ++sourceIndex) {
      IconFrame sourceFrame;
      if (file.read(reinterpret_cast<uint8_t*>(&sourceFrame),
                    sizeof(sourceFrame)) != sizeof(sourceFrame)) {
        discardInvalid();
        return false;
      }
      crc = cacheCrc(reinterpret_cast<const uint8_t*>(&sourceFrame),
                     sizeof(sourceFrame), crc);
      const uint8_t destination = sourceIndex;
      if (delays[destination] == 0) {
        ::memcpy(icon.frames[destination].pixels, sourceFrame.pixels,
                 sizeof(sourceFrame.pixels));
      }
      delays[destination] += max<uint16_t>(sourceFrame.delayMs, 40);
    }
    file.close();
    for (uint8_t index = 0; index < residentFrames; ++index) {
      icon.frames[index].delayMs = static_cast<uint16_t>(
          min<uint32_t>(5000, max<uint32_t>(40, delays[index])));
    }
    if (crc != header.reserved) {
      const bool removed = SPIFFS.exists(path) && SPIFFS.remove(path);
      if (removed && cachedFileCount_ > 0) {
        --cachedFileCount_;
        cachedFileBytes_ =
            cachedFileBytes_ >= invalidBytes
                ? cachedFileBytes_ - invalidBytes
                : 0;
      }
      failedIconId_ = iconId;
      failedIconAtMs_ = 0;
      syncScanPending_ = true;
      return false;
    }
  }
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  addResidentLocked(std::move(icon));
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  return true;
}

uint32_t LaMetricIcons::calculateDominant(
    const std::vector<IconFrame>& frames) {
  uint16_t histogram[512]{};
  for (const IconFrame& frame : frames) {
    for (const CRGB& pixel : frame.pixels) {
      const uint8_t maximum = max(pixel.r, max(pixel.g, pixel.b));
      const uint8_t minimum = min(pixel.r, min(pixel.g, pixel.b));
      if (maximum < 32 || maximum - minimum < 24) {
        continue;
      }
      const uint16_t bucket =
          ((pixel.r >> 5) << 6) | ((pixel.g >> 5) << 3) | (pixel.b >> 5);
      ++histogram[bucket];
    }
  }
  uint16_t bestBucket = 0;
  uint16_t bestCount = 0;
  for (uint16_t index = 0; index < 512; ++index) {
    if (histogram[index] > bestCount) {
      bestCount = histogram[index];
      bestBucket = index;
    }
  }
  if (bestCount == 0) {
    return 0xffffff;
  }
  const uint8_t red = ((bestBucket >> 6) & 7) * 32 + 16;
  const uint8_t green = ((bestBucket >> 3) & 7) * 32 + 16;
  const uint8_t blue = (bestBucket & 7) * 32 + 16;
  return (static_cast<uint32_t>(red) << 16) |
         (static_cast<uint32_t>(green) << 8) | blue;
}

uint32_t LaMetricIcons::dominantColor(uint32_t iconId,
                                      uint32_t fallback) const {
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  uint32_t result = fallback;
  for (const CachedIcon& icon : icons_) {
    if (icon.id == iconId) {
      result = icon.dominant;
      break;
    }
  }
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  if (result == fallback && iconId != 0) {
    ScopedSemaphoreLock fsLock(fsMutex_);
    File file = SPIFFS.open(cachePath(iconId), FILE_READ);
    CacheHeader header{};
    if (file &&
        file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) ==
            sizeof(header) &&
        header.magic == kCacheMagic && header.version == kCacheVersion &&
        header.iconId == iconId && header.frameCount != 0 &&
        header.frameCount <= kMaximumFrames) {
      result = header.dominant;
    }
    if (file) file.close();
  }
  return result;
}

bool LaMetricIcons::draw(uint32_t iconId, PixelDisplay& display,
                         int16_t x, int16_t y, uint32_t nowMs) {
  if (iconId == 0) {
    return false;
  }
  if (failedIconId_ == iconId) return false;
  if (mutex_ != nullptr) {
    xSemaphoreTake(mutex_, portMAX_DELAY);
  }
  const CachedIcon* selected = nullptr;
  for (const CachedIcon& icon : icons_) {
    if (icon.id == iconId) {
      selected = &icon;
      break;
    }
  }
  if (selected == nullptr || selected->frames.empty()) {
    if (mutex_ != nullptr) {
      xSemaphoreGive(mutex_);
    }
    if (!loadCached(iconId)) return false;
    if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
    for (const CachedIcon& icon : icons_) {
      if (icon.id == iconId && !icon.frames.empty()) {
        selected = &icon;
        break;
      }
    }
    if (selected == nullptr) {
      if (mutex_ != nullptr) xSemaphoreGive(mutex_);
      return false;
    }
  }

  uint32_t cycleMs = 0;
  for (const IconFrame& frame : selected->frames) {
    cycleMs += max<uint16_t>(frame.delayMs, 40);
  }
  uint32_t phase = cycleMs == 0 ? 0 : nowMs % cycleMs;
  const IconFrame* frame = &selected->frames.front();
  for (const IconFrame& candidate : selected->frames) {
    const uint16_t delay = max<uint16_t>(candidate.delayMs, 40);
    if (phase < delay) {
      frame = &candidate;
      break;
    }
    phase -= delay;
  }
  for (uint8_t py = 0; py < 8; ++py) {
    for (uint8_t px = 0; px < 8; ++px) {
      display.setPixel(x + px, y + py, frame->pixels[py * 8 + px]);
    }
  }
  if (mutex_ != nullptr) {
    xSemaphoreGive(mutex_);
  }
  return true;
}

bool LaMetricIcons::copyFrame(uint32_t iconId, CRGB output[64],
                              uint32_t nowMs) {
  if (iconId == 0 || output == nullptr) return false;
  if (failedIconId_ == iconId) return false;
  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
  const CachedIcon* selected = nullptr;
  for (const CachedIcon& icon : icons_) {
    if (icon.id == iconId && !icon.frames.empty()) {
      selected = &icon;
      break;
    }
  }
  if (selected == nullptr) {
    if (mutex_ != nullptr) xSemaphoreGive(mutex_);
    if (!loadCached(iconId)) return false;
    if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
    for (const CachedIcon& icon : icons_) {
      if (icon.id == iconId && !icon.frames.empty()) {
        selected = &icon;
        break;
      }
    }
    if (selected == nullptr) {
      if (mutex_ != nullptr) xSemaphoreGive(mutex_);
      return false;
    }
  }
  uint32_t cycleMs = 0;
  for (const IconFrame& frame : selected->frames) {
    cycleMs += max<uint16_t>(frame.delayMs, 40);
  }
  uint32_t phase = cycleMs == 0 ? 0 : nowMs % cycleMs;
  const IconFrame* frame = &selected->frames.front();
  for (const IconFrame& candidate : selected->frames) {
    const uint16_t delay = max<uint16_t>(candidate.delayMs, 40);
    if (phase < delay) {
      frame = &candidate;
      break;
    }
    phase -= delay;
  }
  ::memcpy(output, frame->pixels, sizeof(frame->pixels));
  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
  return true;
}

bool LaMetricIcons::copyFrameByIndex(uint32_t iconId, uint8_t frameIndex,
                                     CRGB output[64], uint16_t& delayMs,
                                     uint8_t& frameCount) {
  if (iconId == 0 || output == nullptr) return false;
  if (failedIconId_ == iconId) return false;
  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
  bool loaded = hasIconLocked(iconId);
  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
  if (!loaded && !loadCached(iconId)) return false;

  if (mutex_ != nullptr) xSemaphoreTake(mutex_, portMAX_DELAY);
  bool copied = false;
  for (const CachedIcon& icon : icons_) {
    if (icon.id != iconId || icon.frames.empty()) continue;
    frameCount = static_cast<uint8_t>(icon.frames.size());
    if (frameIndex < frameCount) {
      const IconFrame& frame = icon.frames[frameIndex];
      ::memcpy(output, frame.pixels, sizeof(frame.pixels));
      delayMs = frame.delayMs;
      copied = true;
    }
    break;
  }
  if (mutex_ != nullptr) xSemaphoreGive(mutex_);
  return copied;
}

}  // namespace pixelos
