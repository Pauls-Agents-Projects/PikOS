#!/usr/bin/env python3
"""Measure PikOS button responsiveness through the API or physical presses."""

from __future__ import annotations

import argparse
import http.client
import json
import math
import sys
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Sequence, Tuple

from pikos_session import open_pikos


class RequestFailure(RuntimeError):
    pass


def normalise_base(host: str) -> str:
    host = host.rstrip("/")
    return host if host.startswith(("http://", "https://")) else "http://" + host


def request(base: str, path: str, *, payload: Optional[dict] = None,
            timeout: float = 5.0, expect_json: bool = True) -> Any:
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    headers = {"Accept": "application/json"}
    if data is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, headers=headers,
                                 method="POST" if data is not None else "GET")
    try:
        with open_pikos(req, timeout=timeout) as response:
            body = response.read()
    except urllib.error.HTTPError as exc:
        raise RequestFailure(f"{path}: HTTP {exc.code}") from exc
    except (urllib.error.URLError, http.client.HTTPException,
            TimeoutError, OSError) as exc:
        raise RequestFailure(f"{path}: {exc}") from exc
    if not expect_json:
        return body
    try:
        result = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RequestFailure(f"{path}: invalid JSON") from exc
    return result if isinstance(result, dict) else {}


def flatten(value: Any, prefix: str = "") -> Dict[str, Any]:
    output: Dict[str, Any] = {}
    if isinstance(value, dict):
        for key, child in value.items():
            output.update(flatten(child, f"{prefix}.{key}" if prefix else str(key)))
    elif prefix:
        output[prefix.lower()] = value
    return output


def pick(flat: Dict[str, Any], *names: str) -> Any:
    for name in names:
        key = name.lower()
        if key in flat:
            return flat[key]
        for source in ("resources", "status", "performance"):
            qualified = f"{source}.{key}"
            if qualified in flat:
                return flat[qualified]
    for name in names:
        matches = [value for key, value in flat.items()
                   if key.endswith("." + name.lower())]
        if matches:
            return matches[0]
    return None


def numeric(value: Any) -> Optional[float]:
    if isinstance(value, bool):
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


@dataclass
class Telemetry:
    uptime: Optional[float]
    boot_id: Optional[str]
    events: Optional[int]
    dropped: Optional[int]
    last_ms: Optional[float]
    p95_ms: Optional[float]
    maximum_ms: Optional[float]
    fps: Optional[float]
    render_frames: Optional[int]
    missed_deadlines: Optional[int]
    loop_gap_ms: Optional[float]


class Client:
    def __init__(self, base: str, timeout: float) -> None:
        self.base = base
        self.timeout = timeout
        self.resources_supported: Optional[bool] = None

    def collect(self, *, resource_only: bool = False) -> Telemetry:
        documents: Dict[str, Any] = {}
        endpoints = (("resources", "/api/v1/resources"),) if (
            resource_only and self.resources_supported is True
        ) else (("status", "/api/v1/status"),
                ("performance", "/api/v1/performance"),
                ("resources", "/api/v1/resources"))
        for label, path in endpoints:
            if label == "resources" and self.resources_supported is False:
                continue
            try:
                documents[label] = request(self.base, path, timeout=self.timeout)
                if label == "resources":
                    self.resources_supported = True
            except RequestFailure as exc:
                if label == "status" or resource_only:
                    raise
                if label == "resources" and "HTTP 404" in str(exc):
                    self.resources_supported = False
        flat = flatten(documents)

        def n(*names: str) -> Optional[float]:
            return numeric(pick(flat, *names))

        boot = pick(flat, "boot_id", "boot_count", "reset_counter")
        events = n("button_event_count", "button_events", "input_event_count")
        drops = n("button_dropped_events", "button_drop_count", "input_drops")
        frames = n("render_frames", "frame_count")
        missed = n("missed_deadlines", "render_missed_deadlines")
        loop_gap = n("max_loop_gap_ms", "loop_max_gap_ms")
        if loop_gap is None:
            loop_gap_us = n("max_frame_gap_us", "render_max_frame_gap_us")
            loop_gap = loop_gap_us / 1000.0 if loop_gap_us is not None else None
        return Telemetry(
            uptime=n("uptime_seconds", "uptime_s", "uptime"),
            boot_id=str(boot) if boot is not None else None,
            events=int(events) if events is not None else None,
            dropped=int(drops) if drops is not None else None,
            last_ms=n("button_last_latency_ms", "last_button_latency_ms",
                      "input_last_latency_ms"),
            p95_ms=n("button_p95_latency_ms", "input_p95_latency_ms"),
            maximum_ms=n("button_max_latency_ms", "input_max_latency_ms"),
            fps=n("render_fps", "fps", "actual_fps"),
            render_frames=int(frames) if frames is not None else None,
            missed_deadlines=int(missed) if missed is not None else None,
            loop_gap_ms=loop_gap,
        )


@dataclass
class PageStressor:
    base: str
    timeout: float
    interval: float
    stop: threading.Event = field(default_factory=threading.Event)
    successes: int = 0
    failures: int = 0

    def run(self) -> None:
        while not self.stop.is_set():
            try:
                request(self.base, "/", timeout=self.timeout, expect_json=False)
                self.successes += 1
            except RequestFailure:
                self.failures += 1
            self.stop.wait(self.interval)


def percentile(values: Sequence[float], fraction: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * fraction) - 1)]


def rebooted(before: Telemetry, after: Telemetry) -> bool:
    if (before.boot_id is not None and after.boot_id is not None
            and before.boot_id != after.boot_id):
        return True
    return (before.uptime is not None and after.uptime is not None
            and after.uptime + 2.0 < before.uptime)


def validate_device(before: Telemetry, after: Telemetry, args: argparse.Namespace,
                    failures: List[str], warnings: List[str]) -> None:
    if rebooted(before, after):
        failures.append("device rebooted during the test")
    if before.dropped is not None and after.dropped is not None:
        if after.dropped > before.dropped:
            failures.append(f"button queue dropped {after.dropped - before.dropped} events")
    else:
        warnings.append("button-drop check unavailable")
    if after.p95_ms is not None and after.p95_ms > args.maximum_device_p95_ms:
        failures.append(
            f"device button p95 {after.p95_ms:.1f} ms exceeds "
            f"{args.maximum_device_p95_ms:.1f} ms")
    if after.maximum_ms is not None and after.maximum_ms > args.maximum_device_ms:
        failures.append(
            f"device button max {after.maximum_ms:.1f} ms exceeds "
            f"{args.maximum_device_ms:.1f} ms")
    if after.fps is not None and after.fps < args.minimum_fps:
        failures.append(f"render rate {after.fps:.1f} FPS is below {args.minimum_fps:.1f}")
    elif after.fps is None:
        warnings.append("render-rate check unavailable")
    if (before.render_frames is not None and after.render_frames is not None
            and before.missed_deadlines is not None
            and after.missed_deadlines is not None):
        rendered = max(0, after.render_frames - before.render_frames)
        missed = max(0, after.missed_deadlines - before.missed_deadlines)
        expected = rendered + missed
        missed_percent = 100.0 * missed / expected if expected else 0.0
        if missed_percent > args.maximum_missed_percent:
            failures.append(
                f"missed-frame rate {missed_percent:.2f}% exceeds "
                f"{args.maximum_missed_percent:.2f}%")
    else:
        warnings.append("missed-frame check unavailable")
    if (after.loop_gap_ms is not None
            and (before.loop_gap_ms is None or after.loop_gap_ms > before.loop_gap_ms)
            and after.loop_gap_ms > args.maximum_loop_gap_ms):
        failures.append(
            f"maximum loop/frame gap {after.loop_gap_ms:.1f} ms exceeds "
            f"{args.maximum_loop_gap_ms:.1f} ms")


def api_mode(client: Client, args: argparse.Namespace,
             before: Telemetry) -> Tuple[List[str], List[str]]:
    failures: List[str] = []
    warnings: List[str] = []
    latencies: List[float] = []
    errors: List[str] = []
    for index in range(args.samples):
        started = time.monotonic()
        try:
            request(client.base, "/api/v1/button", payload={"button": args.button},
                    timeout=args.request_timeout)
            latency = (time.monotonic() - started) * 1000.0
            latencies.append(latency)
            print(f"{index + 1:3d}/{args.samples}: {latency:7.1f} ms")
        except RequestFailure as exc:
            errors.append(str(exc))
            print(f"{index + 1:3d}/{args.samples}: FAILED ({exc})")
        if index + 1 < args.samples:
            time.sleep(args.interval)
    try:
        after = client.collect()
    except RequestFailure as exc:
        failures.append(f"final telemetry failed: {exc}")
        after = before
    p50 = percentile(latencies, 0.50)
    p95 = percentile(latencies, 0.95)
    maximum = max(latencies) if latencies else None
    print("API round trip: success={}/{}, p50={}, p95={}, max={}".format(
        len(latencies), args.samples,
        "n/a" if p50 is None else f"{p50:.1f} ms",
        "n/a" if p95 is None else f"{p95:.1f} ms",
        "n/a" if maximum is None else f"{maximum:.1f} ms"))
    if errors:
        failures.append(f"{len(errors)} of {args.samples} button requests failed")
    if p95 is not None and p95 > args.maximum_api_p95_ms:
        failures.append(f"API p95 {p95:.1f} ms exceeds {args.maximum_api_p95_ms:.1f} ms")
    if maximum is not None and maximum > args.maximum_api_ms:
        failures.append(f"API max {maximum:.1f} ms exceeds {args.maximum_api_ms:.1f} ms")
    validate_device(before, after, args, failures, warnings)
    if after.p95_ms is None:
        warnings.append("device-side edge-to-action latency unavailable; API RTT only")
    return failures, warnings


def physical_mode(client: Client, args: argparse.Namespace,
                  before: Telemetry) -> Tuple[List[str], List[str]]:
    failures: List[str] = []
    warnings: List[str] = []
    if before.events is None:
        print("UNAVAILABLE: firmware does not expose button_event_count in resource telemetry")
        print("Rebuild/flash the telemetry-enabled firmware, then repeat this command.")
        return ["physical observation requires button-event telemetry"], warnings
    target = before.events + args.samples
    deadline = time.monotonic() + args.seconds
    previous_events = before.events
    observed_latencies: List[float] = []
    print(f"Press any physical top button {args.samples} times now.")
    after = before
    while time.monotonic() < deadline and (after.events or 0) < target:
        time.sleep(args.poll_interval)
        try:
            after = client.collect(resource_only=True)
        except RequestFailure:
            continue
        if after.events is not None and after.events > previous_events:
            gained = after.events - previous_events
            if after.last_ms is not None:
                observed_latencies.extend([after.last_ms] * gained)
            previous_events = after.events
            print(f"Observed {after.events - before.events}/{args.samples}; "
                  f"last={after.last_ms if after.last_ms is not None else 'n/a'} ms")
    observed = (after.events or before.events) - before.events
    if observed < args.samples:
        failures.append(f"observed only {observed}/{args.samples} presses in {args.seconds:.0f}s")
    # Polling stays on the compact resource response; collect full performance
    # counters once at the end for the render/deadline assertions.
    try:
        after = client.collect()
    except RequestFailure as exc:
        failures.append(f"final telemetry failed: {exc}")
    validate_device(before, after, args, failures, warnings)
    if observed_latencies:
        p95 = percentile(observed_latencies, 0.95)
        maximum = max(observed_latencies)
        print(f"Observed last-latency samples: p95={p95:.1f} ms, "
              f"max={maximum:.1f} ms")
        if p95 > args.maximum_device_p95_ms:
            failures.append(
                f"observed button p95 {p95:.1f} ms exceeds "
                f"{args.maximum_device_p95_ms:.1f} ms")
        if maximum > args.maximum_device_ms:
            failures.append(
                f"observed button max {maximum:.1f} ms exceeds "
                f"{args.maximum_device_ms:.1f} ms")
    elif after.p95_ms is None:
        warnings.append("latency histogram unavailable; only event delivery was verified")
    return failures, warnings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="PikOS host name, IP address, or base URL")
    parser.add_argument("--mode", choices=("api", "physical"), default="api")
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--button", choices=("next", "previous", "auto"), default="auto")
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--seconds", type=float, default=90.0)
    parser.add_argument("--poll-interval", type=float, default=0.2)
    parser.add_argument("--request-timeout", type=float, default=5.0)
    parser.add_argument("--stress-page", action="store_true")
    parser.add_argument("--page-interval", type=float, default=2.0)
    parser.add_argument("--maximum-api-p95-ms", type=float, default=250.0)
    parser.add_argument("--maximum-api-ms", type=float, default=750.0)
    parser.add_argument("--maximum-device-p95-ms", type=float, default=60.0)
    parser.add_argument("--maximum-device-ms", type=float, default=100.0)
    parser.add_argument("--minimum-fps", type=float, default=60.0)
    parser.add_argument("--maximum-missed-percent", type=float, default=1.0)
    parser.add_argument("--maximum-loop-gap-ms", type=float, default=100.0)
    args = parser.parse_args()
    if args.samples < 1:
        parser.error("--samples must be at least 1")

    client = Client(normalise_base(args.host), args.request_timeout)
    reset_warning: Optional[str] = None
    try:
        request(client.base, "/api/v1/performance/reset", payload={},
                timeout=args.request_timeout)
    except RequestFailure as exc:
        reset_warning = (
            "telemetry reset unavailable; lifetime p95/max may include older presses "
            f"({exc})")
    try:
        before = client.collect()
    except RequestFailure as exc:
        print(f"FAIL: cannot reach device: {exc}", file=sys.stderr)
        return 2

    stressor: Optional[PageStressor] = None
    thread: Optional[threading.Thread] = None
    failures: List[str] = []
    warnings: List[str] = []
    if reset_warning:
        warnings.append(reset_warning)
    if args.stress_page:
        stressor = PageStressor(client.base, args.request_timeout, args.page_interval)
        thread = threading.Thread(target=stressor.run, name="page-stress", daemon=True)
        thread.start()
    try:
        if args.mode == "api":
            failures, warnings = api_mode(client, args, before)
        else:
            failures, warnings = physical_mode(client, args, before)
    finally:
        if stressor is not None and thread is not None:
            stressor.stop.set()
            thread.join(timeout=args.request_timeout + 1.0)
            total = stressor.successes + stressor.failures
            print(f"Settings-page stress: {stressor.successes}/{total} successful")
            if stressor.failures:
                failures.append(f"{stressor.failures} settings-page requests failed")

    for warning in dict.fromkeys(warnings):
        print(f"SKIP/WARN: {warning}")
    if failures:
        for failure in dict.fromkeys(failures):
            print(f"FAIL: {failure}")
        if args.mode == "physical" and before.events is None:
            return 2
        return 1
    print("PASS: button responsiveness stayed within the selected thresholds")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
