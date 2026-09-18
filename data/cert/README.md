# HTTPS trust bundle

`x509_crt_bundle.bin` contains the public Mozilla root certificates distributed
by Espressif for ESP-IDF. PikOS embeds this compact bundle so that its HTTPS
clients verify provider identities instead of accepting arbitrary certificates.

- CA source: Espressif `esp-idf/components/mbedtls/esp_crt_bundle/cacrt_all.pem`
- Bundle generator: Arduino-ESP32 2.0.17 `tools/gen_crt_bundle.py`
- Generated: 2026-09-18
- Certificates: 122

The additional certificate is GlobalSign Root R1, downloaded from GlobalSign
and verified against its published SHA-1 fingerprint
`B1:BC:96:8B:D4:F4:9D:62:2A:A8:9A:81:F2:15:01:52:A4:1D:82:9C`. It remains
valid until 2028-01-28 and is required by the Arduino-ESP32 2.0 bundle verifier
for the GTS R4 cross-chain currently served by Elering, Tallinn transport, and
several Estonian feeds. Remove or replace it before that date.

The generator is Apache-2.0 licensed. The root certificates retain their
individual issuer terms and are public trust anchors rather than private keys.
