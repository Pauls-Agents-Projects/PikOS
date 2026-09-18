# Travel app provider and storage design

## Release decision

The release-candidate firmware ships one conservative provider behind a compact
cache contract: **Tallinn stop departures (experimental)**, with no account or
API key, using the City of Tallinn transport site's own live-departure request.
It is disabled by default. General point-to-point routes remain a documented
next step; OpenRouteService is the lowest-friction walk/cycle/drive candidate,
but it requires a free API key and does not justify a live-traffic claim.

Google Routes can be an optional traffic/transit provider for users willing to
enable billing and restrict an API key. There is no credible, no-key,
production-grade general routing service with live traffic among the providers
reviewed. Waze does not expose a server ETA API for an ESP32.

No route fetch runs while the Travel app is rendering. All network work belongs
to the central sync; the app locally counts down already-cached departures and
durations.

## Provider comparison

| Provider | Modes | Traffic / transit | Setup and terms | PikOS role |
|---|---|---|---|---|
| Tallinn `siri-stop-departures.php` | Stop departures | Expected and scheduled stop times | Keyless but undocumented/no SLA | Experimental Tallinn MVP |
| OpenRouteService | Drive, walk, cycle and other OSM profiles | No live traffic in hosted service | Free account/key; current plan limits apply | Future general-duration candidate |
| Google Routes | Drive, walk, cycle, transit | Traffic-aware driving and transit | Billing and restricted API key required; usage is metered | Optional premium-quality provider |
| GraphHopper | Car, bike, foot on the free plan | Do not assume live traffic | Free account/key; current free plan lists 500 credits/day, non-commercial/no guarantee | Optional alternative |
| Valhalla public demo | Drive, walk, cycle, transit profiles | Depends on demo data, no traffic SLA | No key, fair-use demo; published apps should contact operator | Development probe only |
| Nominatim | Geocoding only | None | No key, max 1 request/s, attribution/cache policy, no autocomplete | One-shot setup geocoder only |
| Waze Deep Links / Transport SDK | Opens Waze; approved mobile partners | Waze app can navigate | No server ETA response; SDK is for approved iOS/Android transport partners | Not a clock provider |

Provider quotas and prices change. The settings UI must link to the selected
provider's live terms instead of baking a quota promise into firmware.

## Implemented device contract

```cpp
struct TravelStopItem {
  String label;
  String stopId;
  String line;
  uint32_t iconId;
};

struct TravelDeparture {
  String route;
  String destination;
  uint32_t expectedAt;
  uint32_t scheduledAt;
  bool realtime;
};

struct TravelStopSnapshot {
  String label;
  String stopId;
  String line;
  uint32_t iconId;
  uint32_t fetchedAt;
  TravelDeparture departures[4];
  uint8_t departureCount;
  String error;
};
```

The implemented release cap is 16 Tallinn stop/line rows. Even at that cap,
fixed records and snapshots remain small and bounded. Do not retain directions, full
provider JSON, map tiles, encoded polylines or a route outline in phase one.

The Tallinn MVP has no credentials and sends only the selected public SIRI stop
IDs to the endpoint during an explicit central sync. Future route origins and
destinations would be personal location data and must stay out of telemetry and
logs.

## Tallinn stop-departure MVP

### Why this route is usable

The official [Tallinn routes and schedules site](https://transport.tallinn.ee/)
currently configures and calls:

```text
GET https://transport.tallinn.ee/siri-stop-departures.php
    ?stopid=<SIRI stop id>&time=<Date.now() milliseconds>
```

The site's own [planner JavaScript](https://transport.tallinn.ee/planner.js?2026-05-09c)
maps its selected stop to `siriID`, sends that URL and parses a small CSV-like
response. The `time` parameter is a cache buster, not a historical-time query.
This endpoint is first-party and keyless, but it is undocumented implementation
detail. It may change without notice, so PikOS must label the provider
“Tallinn — experimental,” preserve last-good data and never tight-loop retries.

### Observed compact format

An official endpoint response captured on 2026-08-25 is reproduced below. It
is truncated after three public departure rows; request headers and the
remaining rows were omitted. The stop ID and destinations are public timetable
identifiers, not user/device data.

```csv
Transport,RouteNum,ExpectedTimeInSeconds,ScheduleTimeInSeconds,46548,version20201024
stop,1234
tram,T2,46962,46932,Kopli,414,Z
tram,T1,47236,47226,Kopli,688,Z
tram,T5,47526,47526,Kopli,978,
```

For the Tallinn branch of the official parser:

- header field 5 (`46548`) is the server's current local seconds-of-day;
- a `stop,<id>` row starts a stop block;
- data fields are transport, route, expected seconds-of-day, scheduled
  seconds-of-day, destination, wait seconds and optional flag;
- the expected time is the live value shown to the user;
- expected differing from scheduled indicates a prediction/schedule offset;
- the final `Z` is an accessibility/low-floor-style flag, not a realtime flag.

The response is CSV-shaped but has no quoted-field grammar in the current
parser. PikOS should still reject commas/control characters in saved line and
stop IDs and treat an unexpected field count as malformed.

### Bounded parser

The firmware parser is deliberately bounded and never buffers the body:

1. stream at most 32 KiB, 512 lines and 160 bytes per line, with a six-second
   idle timeout and a 15-second total response wall-clock limit;
2. require the known header prefix and a numeric server seconds-of-day;
3. validate every returned `stop` block and accept data only for configured
   stop IDs;
4. retain the next four rows per configured item matching the optional
   exact, case-insensitive line filter;
5. require expected/scheduled seconds in `0..172799` and wait in `0..86400`;
6. compute `departureEpoch = fetchedAt + waitSeconds`, using the endpoint's
   wait field; cross-check it against expected minus server time with a
   midnight wrap and reject a discrepancy over 120 seconds;
7. compute scheduled epoch from the expected epoch plus
   `scheduledSeconds - expectedSeconds`, normalized across midnight; and
8. publish the new snapshot only after the entire response passes validation.

The local display presents the first two cached rows (countdown, then clock
time) and subtracts the current clock time from `departureEpoch`; it
does not need Wi-Fi to turn `414` seconds into `7m`, then `6m`, and so on.
Expired rows are removed locally. After 600 seconds without a new sync, mark
predicted times stale, render them grey and fall back to cached scheduled times.
This threshold spans the default five-loop central-sync cadence. A user who
needs fresher predictions can choose a one-loop cadence; PikOS does not create
a hidden high-frequency polling task.

The parser proves that the response contains at least one syntactically valid
stop block. Because the endpoint can legitimately return a requested stop block
with zero rows, the current callback-only integration cannot distinguish that
case from an unexpected stop block that also has zero rows. Both publish
bounded empty snapshots. A response containing departure rows but none for a
configured stop fails closed and keeps the last-good cache.

### Exact parser fixtures

Minimum credential-free tests:

| Fixture | Expected result |
|---|---|
| sample above, no filter | `T2` at `fetchedAt+414`, then `T1` at `+688`; first is realtime |
| sample above, filter `T5` | one result at `+978`; `realtime=false` |
| expected `30`, server `86370`, wait `60` | next-day wrap, `fetchedAt+60` |
| expected/scheduled swapped around midnight | scheduled epoch remains within ±24 h of expected |
| wait differs from wrapped expected-server delta by 121 s | reject and keep last-good |
| wrong stop block containing a departure | no publish |
| HTML error page, empty body, overlong line, 513 lines | reject before cache mutation |
| body over 32 KiB or stream longer than 15 seconds | reject and keep last-good |
| more than two valid rows | validate all bounded rows, retain only the earliest two |
| duplicate/earlier departure | sort, de-duplicate, discard already expired row |

An on-device integration probe should additionally record only status, byte
count, parsed-row count and latency—never the full destination response—in the
serial log.

The redacted capture is checked in as
`tests/fixtures/tallinn_departures_2026-08-25.csv`. The firmware parser and its
native tests are `include/TallinnSiriParser.h`, `src/TallinnSiriParser.cpp` and
`tests/native/test_tallinn_siri_parser.cpp`. The earlier dependency-free Python
reference remains in `tools/tallinn_departures.py`, with fixtures in
`tests/test_tallinn_departures.py`.

```sh
python3 -m unittest tests/test_tallinn_departures.py -v
c++ -std=c++11 -Wall -Wextra -Werror -pedantic -Iinclude \
  src/TallinnSiriParser.cpp tests/native/test_tallinn_siri_parser.cpp \
  -o /tmp/pikos-tallinn-parser-test
/tmp/pikos-tallinn-parser-test
```

The native C++ fixture defines the firmware parser contract; the Python test is
an independently readable reference. Neither performs a network request.

### Stop picker without filling the clock

The site's generated
[`regional_data.js`](https://transport.tallinn.ee/data/regional_data.js) stop
index is currently about 475 KiB, over twice PikOS's entire 192 KiB SPIFFS
partition. It must not be downloaded or parsed by the clock.

Load it only in the settings browser, inside a sandboxed opaque-origin iframe
that exposes minimal `ti.loadStops`/`ti.loadRoutes` callbacks and posts a
sanitized stop list to the parent. Cache that index in browser IndexedDB. The
device receives only the chosen display name, SIRI ID, line filter and icon.
Because this executes changing remote script, keep the iframe sandboxed without
`allow-same-origin`; a later hosted, versioned stop-index service would be safer.

## Estonia GTFS and realtime transit

The Estonian Transport Administration publishes a national static GTFS feed.
Its specification says public reuse requires attribution, application data
must be less than seven days old, and the feed updates daily. The nationwide
archive and joins across `stops`, `trips`, `routes`, `calendar` and
`stop_times` are not realistic within 192 KiB SPIFFS.

GTFS Realtime would be a good compact provider when an agency publishes trip
updates: it is Protocol Buffers carrying trip updates, alerts and vehicle
positions. No authoritative public Tallinn GTFS-RT feed URL was confirmed in
this audit. Do not invent or relabel the custom Tallinn CSV endpoint as
GTFS-RT. Adding a generic GTFS-RT decoder is a documented next step only after
a stable official feed and its terms are identified.

## General route-duration providers

### Low-friction default: OpenRouteService

OpenRouteService supports directions profiles suitable for driving, walking
and cycling and offers a free account tier. It requires a key and its hosted
service does not provide live traffic. PikOS should request only duration and
distance, omit geometry/instructions, cap one request per enabled item per
central sync, and show provider attribution/terms in settings.

This is useful for a stable “HOME → WORK · 24m” estimate. It must be labeled as
a route estimate, not “24m in current traffic.”

### Optional live traffic/transit: Google Routes

Google Routes supports traffic-aware driving and public-transit routes, but
requires a Cloud project with billing and an API key or OAuth. Traffic-aware
route computation is a metered SKU. If selected, the UI should guide the user
to restrict the key to the Routes API, set a daily quota, and store it in NVS.
Use a field mask that returns duration and distance only. Prices and monthly
free usage must be linked from Google's current pricing page rather than copied
into the UI.

### Why the other candidates are not defaults

- GraphHopper is a credible key-based alternative, but its free plan is
  non-commercial/no-guarantee and does not justify a “live traffic” claim.
- Valhalla's public FOSSGIS endpoint is explicitly a fair-use demo. A released
  app should arrange use with its operator; self-hosting global routing graphs
  would reintroduce an always-on companion/server.
- Nominatim is only a geocoder. Use it on an explicit “Find address” click,
  identify the app, remain at or below one request per second, cache the result,
  show OpenStreetMap attribution, and never implement autocomplete against the
  public service.
- Waze Deep Links can open navigation but return no ETA. The Waze Transport SDK
  is for approved transport partners in iOS/Android apps and grants no server
  access suitable for the TC001.

## UI and sync behavior

The release-candidate Travel card uses the same add/remove model as time zones
and stocks. Each saved row is exactly
`label<TAB>SIRI stop ID<TAB>optional line<TAB>icon ID`, capped at 16. It shows
the stop label and up to two next departures. Route origin/destination/mode and
provider selection remain future work.

The central sync serializes provider calls with the existing data worker and
publishes snapshots atomically. A rendering example is
`VABADUSE VÄLJAK T2 7m · T1 11m`. Local countdown changes do not count as
network work and must remain responsive with Wi-Fi disabled.

Repeated-sync tests must cover provider timeouts, 401/403/429/5xx, malformed and
oversized bodies, stale last-good rendering, the 16-item cap, zero hidden calls
outside central sync, and the heap/SPIFFS/button/frame gates in `docs/API.md`.

## Primary sources

- [Tallinn official routes and schedules](https://transport.tallinn.ee/)
- [Tallinn planner source that calls and parses the departures endpoint](https://transport.tallinn.ee/planner.js?2026-05-09c)
- [Estonian Transport Administration GTFS specification](https://www.transpordiamet.ee/media/1339/download)
- [GTFS schedule overview](https://gtfs.org/getting-started/create/)
- [GTFS Realtime feed entities](https://gtfs.org/documentation/realtime/feed-entities/overview/)
- [GTFS Realtime best practices](https://gtfs.org/documentation/realtime/realtime-best-practices/)
- [OpenRouteService directions profiles](https://giscience.github.io/openrouteservice/api-reference/endpoints/directions/)
- [OpenRouteService plans](https://openrouteservice.org/plans/)
- [OpenRouteService statement on hosted traffic data](https://ask.openrouteservice.org/t/isochrone-trafic-data/7875)
- [Google Routes usage and billing](https://developers.google.com/maps/documentation/routes/usage-and-billing)
- [Google transit routes](https://developers.google.com/maps/documentation/routes/transit-route)
- [Google Maps Platform pricing](https://developers.google.com/maps/billing-and-pricing/pricing)
- [GraphHopper pricing](https://www.graphhopper.com/pricing/)
- [GraphHopper terms](https://www.graphhopper.com/terms/)
- [Valhalla public API and fair-use note](https://valhalla.github.io/valhalla/)
- [Nominatim public-use policy](https://operations.osmfoundation.org/policies/nominatim/)
- [Waze Deep Links](https://developers.google.com/waze/deeplinks)
- [Waze Transport SDK overview](https://developers.google.com/waze/intro-transport)
