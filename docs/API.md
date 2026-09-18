# PikOS remote API

## Discovery and status

```http
GET /api/v1/status
GET /api/v1/config
GET /api/v1/performance
GET /api/v1/resources
GET /api/v1/content?app=rss
GET /api/v1/content?app=travel
```

Status and configuration reads redact Wi‑Fi, calendar-feed, Netatmo, and market
credentials, including the generated remote notification/frame API key. Treat
access to the settings portal as administrative access.
The compact performance response contains the render target, rolling render
rate, total rendered frames and missed deadlines, worst frame gap, LED frames
sent/skipped, and maximum WS2812 transmission time. It powers the repeatable
development check:

```sh
python3 tools/monitor_framerate.py pikos.local --seconds 60 --stress-ui
```

The framebuffer integrity monitor holds a selected animated app and samples
atomic RGB565 snapshots. It fails on malformed frames, one-frame blanking,
isolated corruption, excessive ticker discontinuities, or a frozen animation;
suspicious before/current/after triplets are saved as enlarged PPM images:

```sh
python3 tools/monitor_visual_integrity.py pikos.local --app rss --seconds 30
python3 tools/monitor_visual_integrity.py pikos.local --app reddit --seconds 30
```

`/api/v1/resources` is available on telemetry-enabled builds. It exposes the
resource, forced-sync lifecycle, storage/cache, and input counters used by the
development stress tests. The test tools tolerate individual fields—or the
whole endpoint—being absent on an older firmware: they print `SKIP/WARN` for
those checks rather than misreporting them as failures.

`/api/v1/content` lazily lists the bounded cache for one app. Supported values
are `rss`, `reddit`, `calendar`, `travel`, `market`, `climate`, and `energy`. The settings
page calls it only when its Notification centre category is selected, so
ordinary status polling never copies every headline/event String or competes
with the display task. `/resources.storage_by_app` provides the corresponding
compact byte estimates used by System Health.

Travel returns its bounded cached stop snapshots rather than starting a
network request. `expected_at` and `scheduled_at` are absolute Unix seconds;
`minutes` is derived locally from the expected time, or from the scheduled time
when the snapshot is stale. Each item contains at most four departures; the
32×8 display presents the first two while the browser can inspect all four:

```json
{
  "ok": true,
  "app": "travel",
  "count": 1,
  "updated_at": 1787664000,
  "stale": false,
  "items": [{
    "label": "Vabaduse väljak",
    "stop_id": "1234",
    "line": "T2",
    "icon_id": 0,
    "fetched_at": 1787664000,
    "stale": false,
    "error": "",
    "departures": [{
      "route": "T2",
      "destination": "Kopli",
      "expected_at": 1787664414,
      "scheduled_at": 1787664384,
      "realtime": true,
      "minutes": 7
    }]
  }]
}
```

### Forced-sync stress

Run five sequential forced synchronisations while periodically loading the
complete settings page and probing the remote buttons:

```sh
python3 tools/stress_sync.py pikos.local --cycles 5
```

For a longer storage/leak soak, or to isolate the synchroniser from browser and
button traffic:

```sh
python3 tools/stress_sync.py 192.168.1.42 --cycles 20
python3 tools/stress_sync.py 192.168.1.42 --cycles 10 --no-page-stress --no-button-stress
```

The first completed sync is the warm-up reference, so legitimate initial data
or icon allocation is not called a leak. Default acceptance thresholds are:

- every forced sync finishes within 300 seconds and the device does not reboot;
- settled free heap remains at least 32 KiB, its low-water mark at least 12
  KiB, and the largest free block at least 16 KiB; after warm-up, free heap and
  the largest block each lose no more than 8 KiB;
- SPIFFS retains at least 32 KiB free and grows no more than 8 KiB after
  warm-up;
- cache file count does not grow after warm-up and the online store grows no
  more than 4 KiB;
- RSS, market, Calendar, Travel (16 stops), and Reddit counts do not exceed their configured caps
  or continue growing after warm-up, and the timestamped electricity cache
  remains bounded to 208 points;
- settings-page requests succeed at least 90% of the time;
- remote-button round-trip p95 is at most 250 ms and maximum is at most 750 ms;
- there are no failed button requests, queue drops, new sync failures, or new
  TLS-contention events.

All limits have corresponding command-line overrides; run the script with
`--help` for the complete list. A nonzero exit status makes it suitable for a
test bench or CI runner.

### Button latency

Measure 30 remote-control calls while repeatedly loading the settings page:

```sh
python3 tools/monitor_button_latency.py pikos.local --mode api --samples 30 --stress-page
```

Observe 30 real top-button presses after flashing a telemetry-enabled build:

```sh
python3 tools/monitor_button_latency.py pikos.local --mode physical --samples 30 --seconds 90
```

The monitor first calls `POST /api/v1/performance/reset`, which clears the
button latency histogram and main-loop maximum so each run measures its own
window instead of inheriting one historic slow press.

API mode passes when every request succeeds, p95 round-trip latency is at most
250 ms, maximum latency is at most 750 ms, and the device does not reboot.
Where device telemetry is available, both modes additionally require a physical
edge-to-action p95 of at most 60 ms, maximum of 100 ms, zero queue drops, at
least 60 FPS, and no more than 1% missed frames. Physical mode counts events
from the firmware rather than relying on human stopwatch timing; it exits with
status 2 when `button_event_count` is unavailable.

The browser digital twin normally downloads a compact semantic scene and then
renders motion locally at the browser's refresh rate:

```http
GET /api/v1/scene?since=SCENE_KEY
GET /api/v1/icon-animation?id=LAMETRIC_ICON_ID
```

The scene carries text, glyph rows, timing, animation style, icon identity, and
optional chart values. Animated LaMetric frames are fetched once and cached by
the browser. Only a new semantic scene crosses Wi-Fi; ticker, slide, flip,
rain, magic-move, chart, and icon motion are interpolated locally. The older
`GET /api/v1/display?format=delta565&since=FRAME_SEQUENCE` RGB565 stream remains
available for diagnostic tools and compatibility clients.

## Notification

```http
POST /api/v1/notify
X-Pixel-Key: API_KEY
Content-Type: application/json

{
  "text": "Server build finished",
  "color": "#65ffb6",
  "duration_ms": 9000,
  "animation": "ticker",
  "beep": true
}
```

`animation` is `static`, `ticker`, `flip`, `slide`, `rainbow`, `rain`, or
`magic`.

## Chimes

```http
POST /api/v1/beep?tone=news&volume=18&pitch=100
POST /api/v1/beep?tone=gran_vals&volume=12&pitch=98
POST /api/v1/beep?tone=soft
```

Tone names are `off`, `click`, `double`, `news`, `rise`, `soft`, `alert`,
`gran_vals`, `hourly`, and `custom1` through `custom6`. The settings page
assigns and tests a tone independently for RSS, energy-band changes, hourly
chimes, Calendar reminders, remote notifications, and buttons. `volume` is
0–100 PWM duty scaling; `pitch` is an 80–120% frequency multiplier.

An RTTTL value can also be auditioned without saving:

```http
POST /api/v1/rtttl
Content-Type: application/json

{"rtttl":"Test:d=8,o=6,b=140:c,d,e,g,2c7","volume":18,"pitch":100}
```

## Counter control

The Counter app is controlled without changing its saved mode or duration:

```http
POST /api/v1/counter
Content-Type: application/json

{"action":"toggle"}
```

Actions are `open`, `start`, `pause`, `toggle`, and `reset`. A successful
request selects Counter, pins it in manual mode, wakes the display, and returns
its current elapsed/remaining state. `GET /api/v1/status` also exposes
`counter_count_down`, `counter_running`, `counter_finished`,
`counter_duration_seconds`, `counter_elapsed_seconds`, and
`counter_remaining_seconds`. The physical centre button toggles Counter only
while that app is selected; a long centre press resets it. Elsewhere those
buttons keep their normal slideshow/address behavior.

## HTTP pixel frame

```http
POST /api/v1/frame
X-Pixel-Key: API_KEY
Content-Type: application/json

{"pixels":[16711680,65280,255,... 256 values total ...]}
```

Colors are `0xRRGGBB` integers in logical row-major order.

The included sender exercises that endpoint without third-party packages:

```sh
python3 tools/send_frame.py --ip 192.168.1.42 --api-key YOUR_KEY \
  --pattern plasma --seconds 20
```

## Precedence and authentication

`/notify` and `/frame` require `X-Pixel-Key`; `remote_source_ip` can
additionally restrict those senders on a shared LAN. Settings, maintenance,
sound tests, buttons, restart, and OTA require the HttpOnly per-boot cookie set
when the portal is opened plus `X-PikOS-Request: 1`. The included diagnostic
tools establish that session automatically; the long-lived remote key is never
copied into browser JavaScript. Telemetry reads remain available on the trusted
LAN. Do not expose port 80 to the internet. Any frame stream expires after
`remote_timeout_ms` and the local app loop resumes. PikOS 0.18 removed the
retired raw UDP, E1.31, and OpenRGB listeners so local apps have more sockets
and heap available.
