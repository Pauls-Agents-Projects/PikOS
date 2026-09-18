#pragma once

#include <Arduino.h>
#include <DNSServer.h>

#include "Config.h"

namespace pixelos {

class NetworkManager {
 public:
  void begin(const AppConfig& config);
  void tick(uint32_t nowMs);
  bool connected() const;
  bool accessPointActive() const { return apActive_; }
  String ipAddress() const;
  String hostName() const { return hostName_; }
  String accessPointName() const { return apName_; }
  String accessPointPassword() const { return apPassword_; }
  bool consumeAccessPointAnnouncement();
  int32_t rssi() const;
  uint8_t lastDisconnectReason() const { return lastDisconnectReason_; }
  String lastDisconnectMessage() const;

 private:
  void startAccessPoint();
  void onConnected();
  void startStationConnection();

  AppConfig config_;
  DNSServer dns_;
  String apName_;
  String apPassword_;
  String hostName_;
  bool apAnnouncementPending_ = false;
  bool apActive_ = false;
  bool connectedHandled_ = false;
  uint32_t connectStartedMs_ = 0;
  uint32_t lastReconnectMs_ = 0;
  volatile uint8_t lastDisconnectReason_ = 0;
};

}  // namespace pixelos
