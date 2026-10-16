#!/usr/bin/env python3
"""Serve the embedded PikOS UI with lightweight mock device endpoints."""

from __future__ import annotations

import argparse
import json
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "include" / "WebUi.h"


def embedded_html() -> bytes:
    source = SOURCE.read_text(encoding="utf-8")
    match = re.search(r'R"HTML\((.*)\)HTML";', source, re.DOTALL)
    if not match:
        raise RuntimeError("WebUi.h raw HTML block was not found")
    return match.group(1).encode()


CONFIG = {
    "schema_version": 28,
    "device_name": "Ulanzi PikOS",
    "language": "en",
    "timezone": "EET-2EEST,M3.5.0/3,M10.5.0/4",
    "wifi_ssid": "Arukas",
    "orientation_180": False,
    "clock_24_hour": True,
    "auto_brightness": True,
    "manual_brightness": 80,
    "minimum_brightness": 10,
    "maximum_brightness": 255,
    "ldr_dark": 80,
    "ldr_bright": 3500,
    "brightness_gamma": 1.4,
    "app_duration_seconds": 10,
    "maximum_app_seconds": 45,
    "scroll_step_ms": 34,
    "maximum_message_chars": 160,
    "sync_every_loops": 5,
    "sleep_enabled": True,
    "sleep_start": "23:00",
    "sleep_end": "07:00",
    "sleep_idle_enabled": True,
    "sleep_idle_minutes": 30,
    "sleep_display_mode": "clock",
    "button_wake_minutes": 15,
    "button_short_left": "previous",
    "button_long_left": "refresh",
    "button_short_middle": "auto",
    "button_long_middle": "device_info",
    "button_short_right": "next",
    "button_long_right": "counter_toggle",
    "hourly_chime_enabled": False,
    "hourly_chime_start": 8,
    "hourly_chime_end": 22,
    "buzzer_volume": 18,
    "buzzer_pitch_percent": 100,
    "buzzer_high_note_boost_percent": 200,
    "notification_tone": "double",
    "button_tone": "click",
    "climate_provider": "netatmo",
    "climate_wind_icon_id": 2298,
    "climate_rain_icon_id": 72,
    "climate_rain_alert_enabled": True,
    "climate_tone": "soft",
    "battery_auto_low_enabled": True,
    "alarm_tone": "soft",
    "alarm_items": "07:15\t31\tMorning\t\t1\n09:30\t96\tWeekend\tcustom1\t1",
    "counter_enabled": True,
    "counter_icon_id": 35196,
    "counter_mode": "down",
    "counter_duration_seconds": 300,
    "counter_tone": "double",
    "custom_rtttl_1": "Northern Lights:d=8,o=6,b=118:c,e,g,2c7,g,e",
    "custom_rtttl_2": "Harbour Bell:d=4,o=5,b=92:g,8p,g,2c6",
    "clock_timezone_items": "Tallinn\ttallinn\t0\nNew York\tnew_york\t0",
    "rss_feed_items": "ERR\thttps://www.err.ee/rss\t0",
    "market_items": "AIR.PA\t0\nEURGBP\t0",
    "calendar_items": "Home\t@0\t0",
    "calendar_maximum_events": 8,
    "calendar_alerts_enabled": True,
    "travel_stop_items": "Home\t10101\t3\t11999\nOffice\t10234\t\t11999",
    "travel_color": "#4f9cff",
    "activity_enabled": True,
    "activity_animation": "ticker",
    "activity_color": "#65ffb6",
    "activity_icon_id": 397,
    "activity_show_steps": True,
    "activity_show_daily_distance": True,
    "activity_show_latest_distance": True,
    "activity_show_latest_duration": False,
    "activity_show_latest_calories": False,
    "garmin_domain": "garmin.com",
    "garmin_display_name": "Paul",
    "has_garmin_client_id": True,
    "has_garmin_refresh_token": True,
    "reddit_items": "eesti\t33701\ntechnology\t33701",
    "reddit_period": "day",
    "reddit_post_count": 5,
}
for app in ("time", "climate", "rss", "market", "energy", "battery",
            "calendar", "travel", "activity", "reddit", "counter"):
    CONFIG[f"{app}_enabled"] = True
    if app != "counter":
        CONFIG[f"{app}_animation"] = "ticker" if app != "battery" else "static"
CONFIG["battery_enabled"] = False
CONFIG["battery_auto_low_enabled"] = True
CONFIG["travel_enabled"] = False


STATUS = {
    "ok": True,
    "wifi_connected": True,
    "ap_active": False,
    "ip": "192.168.1.42",
    "rssi": -58,
    "current_app": "time",
    "app_paused": False,
    "brightness": 80,
    "light_raw": 1200,
    "battery_percent": 72,
    "battery_trend": "stable",
    "battery_charging_likely": False,
    "battery_saver_active": False,
    "climate_available": True,
    "temperature_c": 21.2,
    "humidity_percent": 43,
    "netatmo_available": True,
    "netatmo_temperature_c": 14.8,
    "netatmo_humidity_percent": 68,
    "netatmo_pressure_hpa": 1014,
    "netatmo_module_name": "Garden",
    "netatmo_rain_available": True,
    "netatmo_rain_mm_h": 0.4,
    "netatmo_rain_1h_mm": 0.8,
    "netatmo_rain_24h_mm": 4.3,
    "netatmo_wind_available": True,
    "netatmo_wind_kph": 18.2,
    "netatmo_gust_kph": 31.6,
    "netatmo_wind_degrees": 46,
    "counter_count_down": True,
    "counter_running": False,
    "counter_finished": False,
    "counter_duration_seconds": 300,
    "counter_elapsed_seconds": 73,
    "counter_remaining_seconds": 227,
    "headline_count": 3,
    "market_count": 5,
    "energy_available": True,
    "energy_band": "low",
    "energy_current_cents_kwh": 6.4,
    "calendar_count": 3,
    "travel_count": 2,
    "travel_stale": False,
    "travel_error": "",
    "activity_connected": True,
    "activity_valid": True,
    "activity_stale": False,
    "activity_reconnect_required": False,
    "activity_updated_at": 1787700600,
    "activity_records": 1,
    "activity_steps_today": 8234,
    "activity_distance_today_m": 6740,
    "activity_latest_started_at": 1787659200,
    "activity_latest_distance_m": 7420,
    "activity_latest_duration_sec": 2482,
    "activity_latest_calories_kcal": 516,
    "activity_latest_type": "run",
    "activity_latest_name": "Evening run",
    "activity_error": "",
    "reddit_count": 3,
    "lametric_icons_cached": 31,
    "estimated_loop_duration_ms": 80000,
    "last_loop_duration_ms": 82400,
    "loops_until_sync": 3,
    "reset_reason": "power-on",
    "previous_boot": {
        "available": True,
        "crash_related": True,
        "reset_reason": "watchdog",
        "context": "sync",
        "app": "rss",
        "sync_provider": "calendar",
        "uptime_ms": 3729000,
        "sequence": 19,
    },
}


SCENE = {
    "ok": True,
    "client_render": True,
    "key": 1,
    "elapsed_ms": 0,
    "step_ms": 34,
    "hold_ms": 10000,
    "base_duration_ms": 10000,
    "duration_ms": 10000,
    "page_count": 1,
    "text_width": 23,
    "style": "rainbow",
    "text": "12:34",
    "previous_text": "12:33",
    "color": 0x65FFB6,
    "clock_font": True,
    "icon_label": "",
    "custom_icon_id": 0,
    "linked_icon": False,
    "app_elapsed_ms": 2600,
    "app": "time",
    "paused": False,
    "glyphs": [
        {"c": ord("1"), "w": 3, "r": [2, 6, 2, 2, 2, 2, 7]},
        {"c": ord("2"), "w": 5, "r": [14, 17, 1, 2, 4, 8, 31]},
        {"c": ord(":"), "w": 1, "r": [0, 1, 1, 0, 1, 1, 0]},
        {"c": ord("3"), "w": 5, "r": [30, 1, 1, 14, 1, 1, 30]},
        {"c": ord("4"), "w": 5, "r": [2, 6, 10, 18, 31, 2, 2]},
    ],
    "icon": [],
}


RESOURCES = {
    "ok": True,
    "free_heap_bytes": 74240,
    "minimum_free_heap_bytes": 43120,
    "largest_heap_block_bytes": 37888,
    "heap_total_bytes": 327680,
    "spiffs_used_bytes": 104300,
    "spiffs_total_bytes": 196608,
    "icon_cache_bytes": 88200,
    "icon_cache_count": 31,
    "render_fps": 63.8,
    "missed_deadlines": 2,
    "button_p95_latency_ms": 40,
    "button_dropped_events": 0,
    "sync_cycles_completed": 6,
    "sync_last_duration_ms": 28400,
    "estimated_loop_duration_ms": 82400,
    "last_loop_duration_ms": 82400,
    "loops_until_sync": 3,
    "storage_by_app": {
        "icons": 88200,
        "rss": 11840,
        "climate": 2048,
        "market": 9780,
        "energy": 7140,
        "calendar": 5340,
        "travel": 2800,
        "activity": 1680,
        "reddit": 8320,
    },
    "storage_by_app_total_bytes": 135468,
}


CONTENT = {
    "rss": {
        "ok": True,
        "app": "rss",
        "count": 3,
        "updated_at": 1792176300,
        "items": [
            {"source": "ERR", "title": "Estonia opens a new coastal nature trail", "icon_id": 0, "id": "rss-1", "timestamp": 1792176300},
            {"source": "BBC", "title": "Night trains return to another European route", "icon_id": 0, "id": "rss-2", "timestamp": 1792173900},
            {"source": "The Verge", "title": "A tiny display makes ambient computing feel useful", "icon_id": 0, "id": "rss-3", "timestamp": 1792171500},
        ],
    },
    "reddit": {
        "ok": True,
        "app": "reddit",
        "count": 3,
        "updated_at": 1792175100,
        "items": [
            {"source": "r/eesti", "title": "Sunset over Tallinn after the rain", "icon_id": 33701, "id": "reddit-1", "timestamp": 1792175100},
            {"source": "r/technology", "title": "What is your favourite low-power display project?", "icon_id": 33701, "id": "reddit-2", "timestamp": 1792172700},
            {"source": "r/esp32", "title": "Making network work invisible to a render loop", "icon_id": 33701, "id": "reddit-3", "timestamp": 1792170300},
        ],
    },
    "calendar": {
        "ok": True,
        "app": "calendar",
        "count": 3,
        "updated_at": 1792175700,
        "items": [
            {"source": "Home", "title": "Dinner with friends", "starts_at": 1792337400, "ends_at": 1792344600, "timestamp": 1792337400, "all_day": False, "icon_id": 0},
            {"source": "Work", "title": "Release candidate review", "starts_at": 1792220400, "ends_at": 1792223100, "timestamp": 1792220400, "all_day": False, "icon_id": 0},
            {"source": "Home", "title": "Morning planning", "starts_at": 1792177500, "ends_at": 1792181100, "timestamp": 1792177500, "all_day": False, "icon_id": 0},
        ],
    },
    "travel": {
        "ok": True,
        "app": "travel",
        "count": 2,
        "updated_at": 1787659200,
        "stale": False,
        "error": "",
        "items": [
            {
                "label": "Home", "stop_id": "10101", "line": "3",
                "icon_id": 0, "fetched_at": 1787659200,
                "stale": False, "error": "",
                "departures": [
                    {"route": "3", "destination": "Viru",
                     "expected_at": 1787659440,
                     "scheduled_at": 1787659500,
                     "realtime": True, "minutes": 4},
                    {"route": "3", "destination": "Kadriorg",
                     "expected_at": 1787660100,
                     "scheduled_at": 1787660100,
                     "realtime": False, "minutes": 15},
                    {"route": "3", "destination": "Vana-Lõuna",
                     "expected_at": 1787661000,
                     "scheduled_at": 1787661000,
                     "realtime": False, "minutes": 30},
                    {"route": "3", "destination": "Tondi",
                     "expected_at": 1787661900,
                     "scheduled_at": 1787661900,
                     "realtime": False, "minutes": 45},
                ],
            },
            {
                "label": "Office", "stop_id": "10234", "line": "",
                "icon_id": 0, "fetched_at": 1787659200,
                "stale": False, "error": "",
                "departures": [
                    {"route": "1", "destination": "Kopli",
                     "expected_at": 1787659620,
                     "scheduled_at": 1787659680,
                     "realtime": True, "minutes": 7},
                    {"route": "5", "destination": "Lennujaam",
                     "expected_at": 1787660700,
                     "scheduled_at": 1787660700,
                     "realtime": False, "minutes": 25},
                ],
            },
        ],
    },
    "activity": {
        "ok": True,
        "app": "activity",
        "count": 1,
        "items": [
            {
                "connected": True,
                "valid": True,
                "stale": False,
                "reconnect_required": False,
                "updated_at": 1787700600,
                "steps_today": 8234,
                "distance_today_m": 6740,
                "latest_started_at": 1787659200,
                "latest_distance_m": 7420,
                "latest_duration_sec": 2482,
                "latest_calories_kcal": 516,
                "latest_type": "run",
                "latest_name": "Evening run",
                "error": "",
            }
        ],
    },
}


BATTERY_HISTORY = {
    "ok": True,
    "interval_seconds": 300,
    "sample_count": 18,
    "newest_age_seconds": 46,
    "age_seconds": [5142, 4840, 4538, 4237, 3935, 3632, 3330, 3028, 2726,
                    2424, 2122, 1820, 1518, 1216, 914, 612, 309, 46],
    "percent": [83, 83, 82, 82, 81, 81, 80, 80, 79, 79, 78, 78, 77, 76,
                76, 75, 74, 74],
    "raw": [2525, 2523, 2517, 2514, 2508, 2505, 2499, 2496, 2490, 2487,
            2481, 2478, 2472, 2466, 2463, 2457, 2451, 2448],
    "drain_percent_per_hour": 6.2,
    "estimated_hours_remaining": 11.9,
    "health_available": False,
    "health_reason": "Voltage-only estimate; current and design capacity are unavailable.",
}


MOCK_LOCK = threading.Lock()
MOCK_POSTS: list[dict[str, object]] = []


class Handler(BaseHTTPRequestHandler):
    def send_bytes(self, status: int, media_type: str, payload: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", media_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def send_json(self, value: object, status: int = 200) -> None:
        self.send_bytes(status, "application/json", json.dumps(value).encode())

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        path = parsed.path
        if path == "/":
            self.send_bytes(200, "text/html; charset=utf-8", embedded_html())
        elif path == "/api/v1/config":
            self.send_json(CONFIG)
        elif path == "/api/v1/status":
            self.send_json(STATUS)
        elif path == "/api/v1/resources":
            self.send_json(RESOURCES)
        elif path == "/api/v1/battery-history":
            self.send_json(BATTERY_HISTORY)
        elif path == "/api/v1/content":
            app = parse_qs(parsed.query).get("app", [""])[0]
            value = CONTENT.get(app)
            if value is None:
                self.send_json({"ok": False, "app": app, "count": 0,
                                "error": "unsupported app", "items": []}, 400)
            else:
                self.send_json(value)
        elif path == "/api/v1/scene":
            since = parse_qs(parsed.query).get("since", ["0"])[0]
            if since == str(SCENE["key"]):
                self.send_bytes(204, "text/plain", b"")
            else:
                self.send_json(SCENE)
        elif path == "/api/v1/mock-state":
            with MOCK_LOCK:
                self.send_json({"ok": True, "posts": list(MOCK_POSTS),
                                "config": dict(CONFIG)})
        else:
            self.send_json({"ok": False, "error": "not found"}, 404)

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length) if length else b""
        try:
            body = json.loads(raw) if raw else {}
        except (json.JSONDecodeError, UnicodeDecodeError):
            self.send_json({"ok": False, "error": "invalid JSON"}, 400)
            return
        if not isinstance(body, dict):
            self.send_json({"ok": False, "error": "JSON object required"}, 400)
            return
        with MOCK_LOCK:
            MOCK_POSTS.append({"path": parsed.path, "body": body})
            del MOCK_POSTS[:-40]
            if parsed.path == "/api/v1/config":
                if body.get("clear_garmin_credentials"):
                    CONFIG["has_garmin_client_id"] = False
                    CONFIG["has_garmin_refresh_token"] = False
                    CONFIG["garmin_display_name"] = ""
                for key, value in body.items():
                    if key == "clear_garmin_credentials":
                        continue
                    if key in {"garmin_client_id", "garmin_refresh_token"}:
                        if value:
                            CONFIG[f"has_{key}"] = True
                        continue
                    CONFIG[key] = value
        self.send_json({"ok": True, "restart": False})

    def log_message(self, _format: str, *_args: object) -> None:
        return


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"PikOS UI preview: http://127.0.0.1:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
