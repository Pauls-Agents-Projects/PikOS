#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "AppEngine.h"
#include "Config.h"
#include "DataServices.h"
#include "LaMetricIcons.h"
#include "NetworkManager.h"
#include "Peripherals.h"
#include "RemoteDisplay.h"

namespace pixelos {

// Arduino WebServer ultimately passes an entire response to WiFiClient::write.
// On the ESP32 that call can retry a dead/slow socket for roughly ten seconds,
// freezing buttons and the display loop. Bound each response write so a stale
// browser tab can never monopolise the main task.
class ResponsiveWebServer : public WebServer {
 public:
  explicit ResponsiveWebServer(uint16_t port = 80) : WebServer(port) {}
  void setWriteBudgetMs(uint32_t budgetMs) { writeBudgetMs_ = budgetMs; }

 protected:
  size_t _currentClientWrite(const char* data, size_t length) override;
  size_t _currentClientWrite_P(PGM_P data, size_t length) override;

 private:
  size_t boundedWrite(const uint8_t* data, size_t length);
  uint32_t writeBudgetMs_ = 200;
};

class WebPortal {
 public:
  WebPortal(ConfigStore& configStore, NetworkManager& network,
            Peripherals& peripherals, DataServices& dataServices,
            RemoteDisplay& remoteDisplay, AppEngine& appEngine,
            Buzzer& buzzer, LaMetricIcons& iconStore,
            PixelDisplay& display);
  void begin();
  void tick(uint32_t nowMs);
  void recordMainLoopGap(uint32_t gapMs) {
    if (gapMs > maximumMainLoopGapMs_) maximumMainLoopGapMs_ = gapMs;
  }
  bool clientActive() {
    WiFiClient client = server_.client();
    if (client && client.connected()) return true;
    const uint32_t remaining = networkHoldUntilMs_ - millis();
    // Every hold is at most ten seconds. Bounding the unsigned distance
    // prevents an expired deadline from resurrecting after 24.9 days uptime.
    if (networkHoldUntilMs_ != 0 && remaining > 0 && remaining <= 10000) {
      return true;
    }
    networkHoldUntilMs_ = 0;
    return false;
  }
  bool consumeButtonWakeRequest();

 private:
  bool checkApiKey();
  bool validRequestHost();
  bool hasAdminSession();
  bool checkAdminSession();
  bool checkApiKeyOrAdminSession();
  void sendRootPage();
  void recordWebBreadcrumb();
  bool cacheBrowserConfig();
  bool sendCachedBrowserConfig();
  void sendJson(int status, JsonDocument& document);
  void handleStatus();
  void handlePerformance();
  void handleResources();
  void handleBatteryHistory();
  void handleContent();
  void handleDisplay();
  void handleScene();
  void handleIconAnimation();
  void handleGetConfig();
  void handleSaveConfig();
  void handleBrightnessPreview();
  void handleNotify();
  void handleFrame();
  void handleButton();
  void handleCounter();
  void handleFirmwareUpload();

  ConfigStore& configStore_;
  NetworkManager& network_;
  Peripherals& peripherals_;
  DataServices& dataServices_;
  RemoteDisplay& remoteDisplay_;
  AppEngine& appEngine_;
  Buzzer& buzzer_;
  LaMetricIcons& iconStore_;
  PixelDisplay& display_;
  ResponsiveWebServer server_{80};
  // WebServer dispatch is single-threaded, so status, scene and settings
  // parsing can share one deterministic pool instead of reserving three BSS
  // arenas that can never be live together.
  // Semantic preview scenes can contain dozens of distinct glyph objects.
  // Keep this deterministic BSS allocation large enough for mixed-case
  // RSS/Reddit text plus the split-flap alphabet; no runtime heap is used.
  // Settings parsing is in-place: even a maximum Garmin refresh token stays
  // in the request String rather than this ArduinoJson pool. This fixed arena
  // therefore covers the flat field/variant graph without spending another
  // 8 KiB of scarce ESP32 BSS on duplicated secret bytes.
  StaticJsonDocument<16384> jsonDocument_;
  String jsonResponseBuffer_;
  bool restartPending_ = false;
  bool buttonWakeRequested_ = false;
  uint32_t restartAtMs_ = 0;
  bool updateFailed_ = false;
  bool firmwareAuthorized_ = false;
  bool listening_ = false;
  bool networkWasReady_ = false;
  uint32_t networkHoldUntilMs_ = 0;
  char adminSessionToken_[33]{};
  volatile uint32_t maximumMainLoopGapMs_ = 0;
  CRGB twinBaseline_[hardware::kLedCount]{};
  uint32_t twinBaselineSequence_ = 0;
  bool twinBaselineValid_ = false;
};

}  // namespace pixelos
