#pragma once

#include <WiFiClientSecure.h>

namespace pixelos {

// Arduino-ESP32 2.x resolves hostnames by calling lwIP's raw DNS API from the
// caller's task. PikOS performs HTTPS work on a dedicated worker, so a DNS
// retry can otherwise race lwIP's TCP/IP timer and leave it with an invalid
// UDP PCB. Resolve through getaddrinfo(), whose netconn path marshals the work
// onto the TCP/IP task, then keep the original hostname for SNI and CA checks.
class SafeTlsClient : public WiFiClientSecure {
 public:
  int connect(const char* host, uint16_t port) override;
  int connect(const char* host, uint16_t port, int32_t timeoutMs) override;

 private:
  int connectResolved(const char* host, uint16_t port);
};

}  // namespace pixelos
