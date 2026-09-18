# Garmin Connect activity app

## Release decision

The only practical companionless personal-data route is an **experimental,
unofficial Garmin Connect integration**:

1. a one-time helper on a Mac performs Garmin's changing login and MFA flow;
2. the helper emits only the persistent DI OAuth refresh token, client ID,
   domain and already-resolved display name as copyable settings JSON;
3. PikOS refreshes short-lived access tokens and reads a small activity snapshot
   during the central sync; and
4. the helper is needed again only if Garmin revokes the refresh token or
   changes the private API.

There is no always-on companion in that design. Garmin account passwords and
MFA codes never reach the TC001; only the narrow DI refresh credential needed
for read-only requests is stored there. The integration remains experimental
because it uses a private Garmin interface and must pass the physical resource
and repeated-sync gates in `docs/API.md`.

The supported one-time helper is `tools/pikos_credentials.py`. The older
`tools/garmin_provision.py` USB skeleton remains release-disabled and is kept
only for shared bounds and mock envelope tests.

## Provider feasibility

| Route | Account friction | Direct from ESP32 | Decision |
|---|---:|---:|---|
| Garmin private DI OAuth | One interactive login/MFA | Yes, after copying setup JSON | Experimental MVP |
| Garmin Health/Activity APIs | Approved business integration | Technically | Not a personal setup route |
| Apple Health | Requires an entitled iOS/watchOS app and user consent | No | Needs a phone companion |
| Strava API | Subscription is now a prerequisite for creating an API app | Technically | Do not make the default |
| DECATHLON | Application approval, OAuth client ID and secret | Technically | More account work than Garmin |
| Moves+ | No documented personal read API or export API was found | No supported route | Do not reverse-engineer it |
| GPX/FIT/XLS exports | Manual export | No live data | Import may be added later |

Garmin's official Health API exposes all-day metrics such as steps, while the
official Activity API exposes completed activities. Both are part of Garmin's
approved Connect Developer Program, whose FAQ describes it as an enterprise
business program rather than a self-service personal API. That makes the
official route unsuitable for a single user's clock even though it would be the
best long-term route for a commercial PikOS product.

Apple Health data lives behind HealthKit authorization in an Apple app. There
is no HealthKit REST endpoint an ESP32 can call. A Shortcut or small iPhone app
could push a compact snapshot later, but that is a companion.

## Why the initial Garmin login does not belong on the clock

The current `python-garminconnect` client uses a five-strategy login chain. It
tries mobile and web flows, several TLS fingerprints, Cloudflare/WAF delays and
MFA handling, then validates the issued token against the API tier. Porting
that browser-emulation logic to Arduino would consume scarce flash/RAM and
still break whenever Garmin changes login.

After login, the flow is much smaller. The current client keeps these native
DI OAuth values:

- `di_refresh_token` — persistent credential;
- `di_client_id` — needed for refresh; and
- `di_token` — short-lived access token.

The refresh request is an ordinary form POST to
`https://diauth.garmin.com/di-oauth2-service/oauth/token`, with Basic auth made
from `base64(client_id + ":")` and the form fields `grant_type=refresh_token`,
`client_id`, and `refresh_token`. Garmin may return a rotated refresh token.
This is private behavior observed in the community client, not a Garmin
contract, so it must be isolated behind a provider and allowed to fail closed.

`garth` is not a fallback: its maintainers deprecated it after Garmin changed
authentication. The helper must use `garminconnect >= 0.3.5`; earlier releases
are also excluded by [GHSA-wjhr-76vg-2hvc](https://github.com/cyberjunky/python-garminconnect/security/advisories/GHSA-wjhr-76vg-2hvc),
which concerns unsafe token-file permissions.

## Phase-one data contract

The first version deliberately does not download FIT files, route maps or an
entire activity history. A successful central sync produces one compact record:

```cpp
struct ActivitySnapshot {
  uint32_t updatedAt;
  uint32_t latestStartedAt;
  uint32_t stepsToday;
  uint32_t distanceTodayM;
  uint32_t latestDistanceM;
  uint32_t latestDurationSec;
  uint16_t latestCaloriesKcal;
  uint8_t latestType;
  bool valid;
  bool stale;
};
```

The provider performs at most three small requests in one serialized worker
job:

1. fetch `/userprofile-service/socialProfile` only when a display name is not
   cached;
2. fetch
   `/usersummary-service/usersummary/daily/{displayName}?calendarDate=YYYY-MM-DD`
   for today's steps and distance; and
3. fetch one bounded page from
   `/activitylist-service/activities/search/activities?start=0&limit=8`, then
   retain only the newest read-only activity fields.

The firmware must not copy the library's open-ended date pagination. Eight
activities is a hard response and parsing bound, not a page size that silently
loops. A route outline is a later feature because FIT/GPX download, decoding and
polyline storage have a much larger memory and privacy footprint.

The display can begin with two views:

- `8.2 km · RUN` for the most recent activity; and
- `10 420 STEPS` for today's daily summary.

Distance, duration and step aggregation happen while syncing. Rendering only
reads `ActivitySnapshot`, so no Garmin network work runs during an animation.

## Token storage and lifecycle

The TC001 NVS partition is only `0x5000` (20 KiB) and is shared with existing
secrets. PikOS must use one compact, versioned NVS blob rather than several
journaled strings:

```text
magic | schema | domain | client-id length | refresh-token length |
client-id | refresh-token | CRC32
```

Requirements:

- allow only `garmin.com` (and `garmin.cn` only when explicitly selected);
- reject a credential update that does not fit before changing NVS;
- commit a new or rotated refresh token atomically;
- keep the access token only in RAM;
- never persist username, password, MFA code, access token or full profile;
- never include secrets in logs, configuration reads, status responses, crash
  telemetry or URLs; and
- expose only redacted presence flags, `connected`, `last_sync`, `stale` and a
  local “Remove Garmin” action after the credential write.

On boot or central sync, refresh the access token. If the response rotates the
refresh token, replace the complete blob atomically before using it again.
Decode the access-token JWT expiry only as a scheduling hint; the server still
authenticates the token.

Error policy:

| Result | Action |
|---|---|
| transport error or 5xx | Keep last-good snapshot, mark stale, wait for next central sync |
| 401 | Refresh once and retry the read once |
| `invalid_grant` or repeated 401 | Delete the unusable in-RAM access token and show “Reconnect Garmin”; do not loop-login |
| 429 | Keep last-good data and make no further Garmin request in that sync |
| oversized/truncated/non-JSON body | Abort parsing and keep last-good data |

## Desktop credential helper

Garmin setup needs Python 3.12 or newer and a security-fixed client release:

```sh
python3 -m pip install 'garminconnect>=0.3.5'
python3 tools/pikos_credentials.py garmin
```

Add `--china` only for a `garmin.cn` account. The helper prompts locally for
the account email, a hidden password and, when requested, a hidden MFA code. It
calls `Garmin.login()` without a token-store path, suppresses provider logging,
and replaces unexpected provider exceptions with fixed diagnostics. It never
prints the email, password, MFA code or short-lived access token.

Successful stdout is a single object whose names match the PikOS Activity card:

```json
{
  "garmin_domain": "garmin.com",
  "garmin_client_id": "…",
  "garmin_refresh_token": "…",
  "garmin_display_name": "…"
}
```

Paste those values into Activity while connected to a trusted LAN, save once,
then clear the terminal. The JSON necessarily contains a persistent refresh
credential even though it excludes all login secrets and access tokens. PikOS
configuration reads return only credential-presence flags, not the values.

The helper performs no USB write, creates no token file, and needs no always-on
process. Its tests use fake provider objects only; they never contact Garmin.

### Physical release gates

All of the following must pass on a physical TC001:

1. configuration rejects unsupported domains and overlong fields without
   changing the old credential;
2. power loss during a credential update leaves either the old or complete new
   NVS blob;
3. ESP32 TLS successfully refreshes a real, user-provisioned token;
4. a rotated refresh token survives reboot;
5. profile, daily-summary and bounded activity requests work with native
   headers and no browser cookies;
6. 20 repeated syncs meet the heap, worker-stack, display-frame and button
   latency gates in `docs/API.md`; and
7. captured configuration reads, status responses, crash telemetry and serial
   logs contain no token fragments.

## Resource budget

- NVS: `0x5000` / 20,480 bytes, shared. Target credential blob: below 4 KiB.
- SPIFFS: `0x30000` / 196,608 bytes. Activity snapshot target: below 512 bytes.
- Each OTA application slot is 1,966,080 bytes. The pre-audit build artifact was
  1,375,200 bytes (about 590 KiB headroom), but only the final linked image is a
  release gate; this design does not spend that margin on login emulation.
- Data worker stack: 14,336 bytes. Reuse the one serialized worker; do not add a
  second TLS task.
- Parsing: ArduinoJson filter or streaming field extraction, one activity page,
  no response-body copy kept after the sync.

The normal PikOS gates remain authoritative: settled free heap at least 32 KiB,
low-water mark at least 12 KiB, largest free block at least 16 KiB, and at
least 32 KiB SPIFFS free.

## Test fixtures

Credential-free tests must cover:

- valid daily summary with steps/distance and missing optional fields;
- valid list with zero, one and eight activities;
- a ninth activity is never requested or parsed;
- response-size, nesting and string-length limits;
- 401 → one refresh → success; repeated 401; 429; 5xx; timeout; invalid JSON;
- rotated refresh-token atomic commit and simulated power loss;
- corrupt NVS CRC and unsupported schema;
- stale last-good rendering after every failure class;
- 20 consecutive central syncs with no heap/cache growth; and
- a repository scan proving fixture/log files contain no real token, email,
  location or health data.

## Optional local bridge

A local companion remains an optional alternative for users who do not want a
Garmin refresh token on the clock. It can expose the same compact
`ActivitySnapshot` as authenticated JSON. It is not required by the MVP and
must not become an always-on prerequisite for the direct route.

## Primary and upstream sources

- [Garmin Health API](https://developer.garmin.com/gc-developer-program/health-api/)
- [Garmin Activity API](https://developer.garmin.com/gc-developer-program/activity-api/)
- [Garmin Connect Developer Program FAQ](https://developer.garmin.com/gc-developer-program/program-faq/)
- [`python-garminconnect` authentication and token implementation](https://github.com/cyberjunky/python-garminconnect/blob/master/garminconnect/client.py)
- [`python-garminconnect` daily summary and activity methods](https://github.com/cyberjunky/python-garminconnect/blob/master/garminconnect/__init__.py)
- [`python-garminconnect` security advisory](https://github.com/cyberjunky/python-garminconnect/security/advisories/GHSA-wjhr-76vg-2hvc)
- [Garth releases and deprecation notice](https://github.com/matin/garth/releases)
- [Apple HealthKit access configuration](https://developer.apple.com/documentation/xcode/configuring-healthkit-access)
- [Apple HealthKit authorization](https://developer.apple.com/documentation/healthkit/authorizing-access-to-health-data)
- [Strava API getting started](https://developers.strava.com/docs/getting-started/)
- [Strava OAuth](https://developers.strava.com/docs/authentication/)
- [DECATHLON Sports Tracking Data getting started](https://github.com/Decathlon/sports-tracking-data-documentation/blob/master/docs/020-get-started.md)
- [Moves+ tracker documentation](https://support.openplay.net/hc/en-gb/articles/360020315817-INTRO-TO-TRACKERS-WEARABLES)
