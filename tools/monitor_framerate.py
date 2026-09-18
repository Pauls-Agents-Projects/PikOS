#!/usr/bin/env python3
"""Measure PikOS render cadence through its lightweight performance API.

Example:
  python3 tools/monitor_framerate.py 192.168.1.42 --seconds 60 --stress-ui
"""

from __future__ import annotations

import argparse
import json
import threading
import time
import urllib.request


def fetch_json(url: str, timeout: float = 3.0) -> dict:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.load(response)


def stress_settings_page(base_url: str, stop: threading.Event) -> None:
    while not stop.wait(2.0):
        try:
            with urllib.request.urlopen(base_url + "/", timeout=5.0) as response:
                while response.read(8192):
                    pass
        except Exception:
            # The monitor reports reachability separately. Stress traffic is
            # best-effort so a single Wi-Fi retry does not end the test.
            pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="PikOS hostname or IP address")
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--minimum-fps", type=float, default=60.0)
    parser.add_argument(
        "--maximum-missed-percent", type=float, default=1.0,
        help="Maximum missed render deadlines as a percentage of expected frames",
    )
    parser.add_argument(
        "--stress-ui", action="store_true",
        help="Repeatedly download the settings page while measuring",
    )
    parser.add_argument(
        "--fail-on-network-error", action="store_true",
        help="Also fail when a telemetry request is lost",
    )
    args = parser.parse_args()

    base_url = args.host if "://" in args.host else "http://" + args.host
    performance_url = base_url.rstrip("/") + "/api/v1/performance"
    stop = threading.Event()
    stress_thread = None
    if args.stress_ui:
        stress_thread = threading.Thread(
            target=stress_settings_page,
            args=(base_url.rstrip("/"), stop),
            daemon=True,
        )
        stress_thread.start()

    samples: list[tuple[float, dict]] = []
    failures = 0
    deadline = time.monotonic() + args.seconds
    try:
        while time.monotonic() < deadline:
            started = time.monotonic()
            try:
                sample = fetch_json(performance_url)
                samples.append((time.monotonic(), sample))
                print(
                    f"fps={sample.get('render_fps', 0):5.1f}  "
                    f"missed={sample.get('missed_deadlines', 0):6d}  "
                    f"max-gap={sample.get('max_frame_gap_us', 0) / 1000:7.1f} ms  "
                    f"LED-show={sample.get('led_max_show_us', 0) / 1000:5.1f} ms  "
                    f"sync={'yes' if sample.get('data_refreshing') else 'no'}"
                )
            except Exception as error:
                failures += 1
                print(f"unreachable: {error}")
            time.sleep(max(0.0, args.interval - (time.monotonic() - started)))
    finally:
        stop.set()
        if stress_thread:
            stress_thread.join(timeout=1.0)

    if len(samples) < 2:
        print("FAIL: not enough performance samples")
        return 2

    first_at, first = samples[0]
    last_at, last = samples[-1]
    elapsed = last_at - first_at
    rendered = last["render_frames"] - first["render_frames"]
    missed = last["missed_deadlines"] - first["missed_deadlines"]
    measured_fps = rendered / elapsed
    expected = rendered + missed
    missed_percent = missed * 100.0 / expected if expected else 0.0
    passed = (
        measured_fps >= args.minimum_fps
        and missed_percent <= args.maximum_missed_percent
        and (failures == 0 or not args.fail_on_network_error)
    )
    print(
        f"{'PASS' if passed else 'FAIL'}: {measured_fps:.2f} FPS, "
        f"{missed}/{expected} deadlines missed ({missed_percent:.2f}%), "
        f"{failures} request failure(s)"
    )
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
