# Security policy

PikOS is a local-network appliance, not an internet-facing service. Do not
forward its HTTP port from a router or place it directly on a public network.

## Protections

- HTTPS provider connections verify server identities against an embedded
  public CA bundle.
- Wi-Fi, calendar, Netatmo, Garmin, market, and remote API credentials are
  redacted from browser responses and stored separately from ordinary settings.
- Browser changes require an HttpOnly, `SameSite=Strict` per-boot session plus
  an explicit same-origin request header. OTA authorization is checked before
  the first flash write.
- Remote notification and framebuffer endpoints require a constant-time API-key
  comparison. Responses include a restrictive Content Security Policy,
  anti-framing, MIME-sniffing, referrer, and browser-permission headers.
- The setup access point uses a per-device password shown on the matrix rather
  than a shared factory password.

## Trust boundary

The settings page has no user-account login. Anyone who can join the device's
LAN and deliberately open its settings address can administer it. Use a trusted
home network or an isolated IoT VLAN. HTTP traffic between a browser and PikOS
is not encrypted; provider secrets are never returned, but submitted settings
can be observed by an attacker already controlling that LAN.

The TC001 does not ship with PikOS flash encryption or secure boot. Someone
with physical serial access may be able to extract stored credentials. Use
read-only, revocable tokens where providers support them and keep full-flash
backups private.

## Reporting a vulnerability

Please open a GitHub security advisory for the repository rather than posting
credentials or an exploitable issue publicly. Include the PikOS version,
affected endpoint/provider, reproduction steps, and whether physical or LAN
access is required. Never attach a full TC001 flash dump.

Before a release, maintainers should run:

```sh
python3 tools/security_audit.py
python3 -m unittest discover -s tests -v
pio run
```
