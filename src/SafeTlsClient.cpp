#include "SafeTlsClient.h"

#include <lwip/netdb.h>
#include <lwip/sockets.h>

namespace pixelos {

int SafeTlsClient::connect(const char* host, uint16_t port) {
  return connectResolved(host, port);
}

int SafeTlsClient::connect(const char* host, uint16_t port,
                           int32_t timeoutMs) {
  _timeout = timeoutMs;
  return connectResolved(host, port);
}

int SafeTlsClient::connectResolved(const char* host, uint16_t port) {
  if (host == nullptr || host[0] == '\0') return 0;

  IPAddress address;
  if (!address.fromString(host)) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* results = nullptr;
    const int error = getaddrinfo(host, nullptr, &hints, &results);
    if (error != 0 || results == nullptr || results->ai_addr == nullptr) {
      if (results != nullptr) freeaddrinfo(results);
      return 0;
    }
    const sockaddr_in* resolved =
        reinterpret_cast<const sockaddr_in*>(results->ai_addr);
    address = IPAddress(resolved->sin_addr.s_addr);
    freeaddrinfo(results);
  }

  // This overload connects to the resolved address but passes host onward to
  // mbedTLS. Certificate validation and SNI therefore remain hostname based.
  return WiFiClientSecure::connect(address, port, host, _CA_cert, _cert,
                                   _private_key);
}

}  // namespace pixelos
