#include "NetworkManager.h"

#include <WiFi.h>
#include <esp_wifi.h>

namespace pixelos {
namespace {

String safeHostName(const String& value) {
  String result;
  result.reserve(24);
  for (size_t index = 0; index < value.length() && result.length() < 24;
       ++index) {
    const char c = value[index];
    if (isalnum(static_cast<unsigned char>(c))) {
      result += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    } else if ((c == ' ' || c == '_' || c == '-') && !result.endsWith("-")) {
      result += '-';
    }
  }
  while (result.endsWith("-")) {
    result.remove(result.length() - 1);
  }
  return result.isEmpty() ? "pikos" : result;
}

const char* disconnectReasonText(uint8_t reason) {
  switch (reason) {
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
      return "Password handshake timed out";
    case WIFI_REASON_AUTH_FAIL:
      return "Authentication failed";
    case WIFI_REASON_ASSOC_FAIL:
      return "Router association failed";
    case WIFI_REASON_NO_AP_FOUND:
      return "Wi-Fi network not found on 2.4 GHz";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "Router signal was lost";
    case WIFI_REASON_CONNECTION_FAIL:
      return "Connection failed";
    default:
      return reason == 0 ? "Waiting for Wi-Fi" : "Wi-Fi disconnected";
  }
}

}  // namespace

void NetworkManager::begin(const AppConfig& config) {
  config_ = config;
  setenv("TZ", config_.timezone.c_str(), 1);
  tzset();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  // ESP32 defaults may exclude European channels 12 and 13 until country
  // information is received from an access point. Set Estonia explicitly so
  // the station can discover a router on either channel during first contact.
  const esp_err_t countryResult = esp_wifi_set_country_code("EE", true);
  if (countryResult != ESP_OK) {
    Serial.printf("[wifi] Could not set EE radio domain: %d\n",
                  static_cast<int>(countryResult));
  }
  WiFi.onEvent(
      [this](arduino_event_id_t event, arduino_event_info_t info) {
        if (event != ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
          return;
        }
        lastDisconnectReason_ = info.wifi_sta_disconnected.reason;
        Serial.printf("[wifi] Disconnected: %u (%s)\n",
                      static_cast<unsigned>(lastDisconnectReason_),
                      disconnectReasonText(lastDisconnectReason_));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  hostName_ = safeHostName(config.deviceName);
  WiFi.setHostname(hostName_.c_str());
  connectStartedMs_ = millis();

  if (!config.wifiSsid.isEmpty()) {
    startStationConnection();
  } else {
    startAccessPoint();
  }
}

void NetworkManager::startStationConnection() {
  connectStartedMs_ = millis();
  lastReconnectMs_ = millis();
  WiFi.begin(config_.wifiSsid.c_str(), config_.wifiPassword.c_str());
  Serial.printf("[wifi] Connecting to %s (password %s)\n",
                config_.wifiSsid.c_str(),
                config_.wifiPassword.isEmpty() ? "missing" : "saved");
}

void NetworkManager::tick(uint32_t nowMs) {
  // Connection health must stay a constant-time status check. The former
  // TCP/53 gateway probe could block this latency-critical main task for
  // 900 ms every 30 seconds, delaying physical button recognition even when
  // the display renderer itself remained smooth.
  if (connected()) {
    if (!connectedHandled_) {
      onConnected();
    }
  } else {
    connectedHandled_ = false;
    if (!apActive_ && nowMs - connectStartedMs_ >= 15000) {
      startAccessPoint();
    }
    if (!config_.wifiSsid.isEmpty() &&
        nowMs - lastReconnectMs_ >= 30000) {
      startStationConnection();
    }
  }
  if (apActive_) {
    dns_.processNextRequest();
  }
}

void NetworkManager::startAccessPoint() {
  uint8_t mac[6]{};
  WiFi.macAddress(mac);
  char suffix[7];
  snprintf(suffix, sizeof(suffix), "%02X%02X%02X", mac[3], mac[4], mac[5]);
  apName_ = "PikOS-";
  apName_ += suffix;
  const size_t keyLength = config_.apiKey.length();
  apPassword_ = keyLength >= 12 ? config_.apiKey.substring(keyLength - 12)
                                : String("pikos-") + suffix;
  WiFi.mode(WIFI_AP_STA);
  const bool started =
      WiFi.softAP(apName_.c_str(), apPassword_.c_str(), 1, false, 4);
  dns_.start(53, "*", WiFi.softAPIP());
  apActive_ = started;
  apAnnouncementPending_ = started;
  Serial.printf("[wifi] Setup AP %s %s at %s\n", apName_.c_str(),
                started ? "ready" : "failed",
                WiFi.softAPIP().toString().c_str());
}

bool NetworkManager::consumeAccessPointAnnouncement() {
  const bool pending = apAnnouncementPending_;
  apAnnouncementPending_ = false;
  return pending;
}

void NetworkManager::onConnected() {
  connectedHandled_ = true;
  if (apActive_ && !config_.wifiSsid.isEmpty()) {
    dns_.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apActive_ = false;
    apAnnouncementPending_ = false;
  }
  configTzTime(config_.timezone.c_str(), "pool.ntp.org",
               "time.cloudflare.com", "time.google.com");

  // PikOS exposes the current IP in the status pane and on a long middle
  // press. Avoid running ESP-IDF's mDNS receiver alongside memory-heavy TLS
  // handshakes: under severe heap pressure its asynchronous logging path can
  // attempt to create a newlib lock and abort the whole ESP32.
  Serial.printf("[wifi] Connected: %s\n", WiFi.localIP().toString().c_str());
  lastDisconnectReason_ = 0;
}

bool NetworkManager::connected() const {
  return WiFi.status() == WL_CONNECTED &&
         static_cast<uint32_t>(WiFi.localIP()) != 0;
}

String NetworkManager::ipAddress() const {
  if (connected()) {
    return WiFi.localIP().toString();
  }
  if (apActive_) {
    return WiFi.softAPIP().toString();
  }
  return "0.0.0.0";
}

int32_t NetworkManager::rssi() const {
  return connected() ? WiFi.RSSI() : 0;
}

String NetworkManager::lastDisconnectMessage() const {
  return disconnectReasonText(lastDisconnectReason_);
}

}  // namespace pixelos
