# Changelog

## 0.22.0-rc8 — 2026-10-25

- Decoded the first on-device core dump and fixed the shared provider crash:
  outbound TLS clients now use lwIP's thread-safe resolver instead of racing
  the TCP/IP DNS retry timer from the online worker.
- Gave the single WS2812 panel all eight hardware RMT memory blocks, cutting
  refill interrupts by 4×, and serialized flash writes around each LED
  transfer to prevent random one-frame colours during Wi-Fi/cache activity.
- Added animated LaMetric icon 8268 to the low-light StandBy clock, with an
  offline built-in fallback.
- Restyled the Notification Centre as one chronological visual timeline and
  replaced the connected-status dot with accessible signal-strength bars.
- Documented a safe declarative GitHub app-catalog direction; arbitrary native
  plug-ins remain intentionally out of the release candidate.

## 0.22.0-rc7 — 2026-10-16

- Merged cached RSS, calendar, Reddit, travel, and activity entries into one
  chronological Notification Centre feed with per-app filters.
- Added RSS/Atom publication timestamps and missing square-bracket glyphs.
- Added per-provider Sync circuit breakers after a panic or three consecutive
  ordinary failures, keeping unrelated apps operational and identifying the
  paused source in System health.
- Added a 128 KiB ELF core-dump partition while preserving SPIFFS in place.
- Documented the four retained long-uptime Sync panics and safe dump handling.

## 0.22.0-rc6 — 2026-09-18

First public release candidate of PikOS.

### Highlights

- Local-first clock, climate, RSS, markets, energy, calendar, Reddit, travel,
  activity, battery, alarms, stopwatch, countdown, and explicit Sync apps.
- Smooth 64 fps display task, responsive button queue, semantic browser twin,
  animated LaMetric icon cache, seven text transitions, and Estonian UI.
- New main-page notification centre for cached headlines, events, posts,
  departures, and activity; full-height horizontal app carousel with desktop
  wheel navigation.
- Persistent four-entry crash journal for panic, watchdog, and brownout resets.
- Release hardening: verified HTTPS CA bundle, redacted remote key, dedicated
  credential vault, per-device setup password, same-origin browser sessions,
  protected settings/maintenance/OTA routes, and restrictive response headers.

### Known constraints

- The portal is designed for a trusted LAN and has no multi-user login.
- Garmin Connect and Tallinn transit support use unofficial/experimental
  interfaces and may need maintenance when upstream services change.
- Battery state of charge is an estimate because the TC001 exposes voltage,
  not a fuel-gauge IC.
