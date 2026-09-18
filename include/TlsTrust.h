#pragma once

class WiFiClientSecure;

namespace pixelos {

// Attach PikOS's embedded public CA bundle and bounded handshake timeout.
// All HTTPS clients must pass through this helper; never call setInsecure().
void configureTrustedTlsClient(WiFiClientSecure& client);

}  // namespace pixelos
