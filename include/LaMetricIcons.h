#pragma once

#include <Arduino.h>
#include <FastLED.h>

#include <vector>

#include "Config.h"

namespace pixelos {

class PixelDisplay;

constexpr uint32_t kStandbyClockIconId = 8268;

// Downloads selected 8x8 LaMetric gallery icons, stores a compact binary copy
// in SPIFFS, and renders animated frames without blocking the display loop.
class LaMetricIcons {
 public:
  bool begin(const AppConfig& config);
  void tick(bool wifiConnected, const AppConfig& config, uint32_t nowMs);
  bool draw(uint32_t iconId, PixelDisplay& display, int16_t x, int16_t y,
            uint32_t nowMs);
  bool copyFrame(uint32_t iconId, CRGB output[64], uint32_t nowMs);
  bool copyFrameByIndex(uint32_t iconId, uint8_t frameIndex,
                        CRGB output[64], uint16_t& delayMs,
                        uint8_t& frameCount);
  uint32_t faviconId(const String& feedUrl) const;
  uint32_t dominantColor(uint32_t iconId, uint32_t fallback) const;
  bool ready(uint32_t iconId) const;
  bool downloading() const { return downloading_; }
  void requestSyncScan() { syncScanPending_ = true; }
  void reconcileCache(const AppConfig& config);
  bool radioBusy() const {
    const uint32_t remaining = networkCooldownUntilMs_ - millis();
    return syncScanPending_ || downloading_ ||
           (networkCooldownUntilMs_ != 0 && remaining > 0 &&
            remaining <= 1000);
  }
  size_t cachedCount() const;
  size_t cachedBytes() const;
  String lastError() const;

 private:
  struct IconFrame {
    uint16_t delayMs = 250;
    CRGB pixels[64]{};
  };

  struct CachedIcon {
    uint32_t id = 0;
    uint32_t dominant = 0xffffff;
    std::vector<IconFrame> frames;
  };

  static void downloadTask(void* parameter);
  bool download(uint32_t iconId);
  bool downloadFavicon(uint32_t iconId, const String& feedUrl);
  bool downloadPngIcon(uint32_t iconId, const String& url,
                       const char* description, uint8_t attempts = 2);
  bool loadCached(uint32_t iconId);
  bool saveCached(const CachedIcon& icon);
  bool hasIconLocked(uint32_t iconId) const;
  void addResidentLocked(CachedIcon&& icon);
  void setLastError(const String& error);
  static uint32_t calculateDominant(const std::vector<IconFrame>& frames);
  static String cachePath(uint32_t iconId);

  mutable SemaphoreHandle_t mutex_ = nullptr;
  // Resident animation data and flash I/O have different contention
  // profiles. Keep normal 64 fps draws independent from an icon download,
  // while serialising every SPIFFS scan/read/write/remove across tasks.
  mutable SemaphoreHandle_t fsMutex_ = nullptr;
  std::vector<CachedIcon> icons_;
  // Flip and app transitions can need the outgoing and incoming animations
  // in the same physical frame. Retaining two avoids SPIFFS load/evict churn
  // in the 50 fps render path (about 12 KiB in the worst 32-frame case).
  static constexpr size_t kMaximumResidentIcons = 2;
  static constexpr size_t kMaximumResidentFrames = 32;
  size_t cachedFileCount_ = 0;
  size_t cachedFileBytes_ = 0;
  bool fileSystemReady_ = false;
  volatile bool downloading_ = false;
  volatile bool syncScanPending_ = false;
  volatile uint32_t networkCooldownUntilMs_ = 0;
  // The download worker is created only when an icon is actually missing and
  // deletes itself afterwards. Its TLS/PNG stack no longer consumes RAM for
  // the other 99.9% of uptime.
  static constexpr uint32_t kWorkerStackBytes = 12288;
  TaskHandle_t workerTask_ = nullptr;
  uint32_t pendingIconId_ = 0;
  String pendingSourceUrl_;
  uint32_t lastAttemptMs_ = 0;
  volatile uint32_t lastFailureMs_ = 0;
  volatile uint32_t failedIconId_ = 0;
  volatile uint32_t failedIconAtMs_ = 0;
  String lastError_;
};

}  // namespace pixelos
