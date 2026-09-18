#include "TlsTrust.h"

#include <WiFiClientSecure.h>
#include <cstdint>

namespace pixelos {
namespace {

extern const uint8_t kRootCaBundle[]
    asm("_binary_data_cert_x509_crt_bundle_bin_start");

}  // namespace

void configureTrustedTlsClient(WiFiClientSecure& client) {
  client.setCACertBundle(kRootCaBundle);
  client.setHandshakeTimeout(6);
}

}  // namespace pixelos
