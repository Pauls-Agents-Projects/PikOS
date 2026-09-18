# Changelog

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
