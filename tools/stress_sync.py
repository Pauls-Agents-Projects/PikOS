#!/usr/bin/env python3
"""Stress PikOS forced synchronisation and verify resources recover each cycle.

Only the Python standard library is required. Detailed checks switch on when
the resource telemetry endpoint exists; older builds report individual checks
as skipped instead of crashing or producing a false failure.
"""

from __future__ import annotations

import argparse
import http.client
import json
import math
import statistics
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
            timeout: float = 8.0, expect_json: bool = True) -> Any:
    data = None
    headers = {"Accept": "application/json"}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(base + path, data=data, headers=headers,
                                 method="POST" if data is not None else "GET")
    try:
        with open_pikos(req, timeout=timeout) as response:
            body = response.read()
    except urllib.error.HTTPError as exc:
        detail = exc.read(200).decode("utf-8", "replace")
        raise RequestFailure(f"{path}: HTTP {exc.code} {detail}".strip()) from exc
    except (urllib.error.URLError, http.client.HTTPException,
            TimeoutError, OSError) as exc:
        raise RequestFailure(f"{path}: {exc}") from exc
    if not expect_json:
        return body
    try:
        value = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise RequestFailure(f"{path}: invalid JSON ({exc})") from exc
    if not isinstance(value, dict):
        raise RequestFailure(f"{path}: expected a JSON object")
    return value


def flatten(value: Any, prefix: str = "") -> Dict[str, Any]:
    result: Dict[str, Any] = {}
    if isinstance(value, dict):
        for key, child in value.items():
            path = f"{prefix}.{key}" if prefix else str(key)
            result.update(flatten(child, path))
    elif prefix:
        result[prefix.lower()] = value
    return result


def pick(flat: Dict[str, Any], names: Sequence[str]) -> Any:
    for name in names:
        key = name.lower()
        if key in flat:
            return flat[key]
        for source in ("resources", "status", "performance"):
            qualified = f"{source}.{key}"
            if qualified in flat:
                return flat[qualified]
    for name in names:
        suffix = "." + name.lower()
        matches = [value for key, value in flat.items() if key.endswith(suffix)]
        if matches:
            return matches[0]
    return None


def number(value: Any) -> Optional[float]:
    if isinstance(value, bool):
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) else None


def integer(value: Any) -> Optional[int]:
    parsed = number(value)
    return int(parsed) if parsed is not None else None


class TelemetryClient:
    def __init__(self, base: str, timeout: float) -> None:
        self.base = base
        self.timeout = timeout
        self.resources_supported: Optional[bool] = None
        self.performance_supported: Optional[bool] = None
        self.notices: List[str] = []

    def _optional(self, path: str, flag_name: str) -> dict:
        if getattr(self, flag_name) is False:
            return {}
        try:
            result = request(self.base, path, timeout=self.timeout)
            setattr(self, flag_name, True)
            return result
        except RequestFailure as exc:
            text = str(exc)
            if "HTTP 404" in text:
                setattr(self, flag_name, False)
                self.notices.append(f"{path} is not present; related checks will be skipped")
            else:
                self.notices.append(f"temporary telemetry failure: {text}")
            return {}

    def collect(self, *, include_resources: bool = True) -> "Snapshot":
        status = request(self.base, "/api/v1/status", timeout=self.timeout)
        performance = self._optional("/api/v1/performance", "performance_supported")
        resources = (self._optional("/api/v1/resources", "resources_supported")
                     if include_resources else {})
        return Snapshot.from_flat(flatten({
            "status": status, "performance": performance, "resources": resources,
        }))


@dataclass
class Snapshot:
    taken: float
    uptime: Optional[float] = None
    boot_id: Optional[str] = None
    render_frames: Optional[int] = None
    free_heap: Optional[int] = None
    largest_block: Optional[int] = None
    minimum_heap: Optional[int] = None
    spiffs_used: Optional[int] = None
    spiffs_total: Optional[int] = None
    cache_bytes: Optional[int] = None
    cache_count: Optional[int] = None
    rss_count: Optional[int] = None
    market_count: Optional[int] = None
    energy_count: Optional[int] = None
    calendar_count: Optional[int] = None
    travel_count: Optional[int] = None
    reddit_count: Optional[int] = None
    online_store_bytes: Optional[int] = None
    sync_failures: Optional[int] = None
    tls_contention: Optional[int] = None
    button_drops: Optional[int] = None
    sync_active: Optional[bool] = None
    sync_cycles_completed: Optional[int] = None

    @classmethod
    def from_flat(cls, flat: Dict[str, Any]) -> "Snapshot":
        def n(*names: str) -> Optional[int]:
            return integer(pick(flat, names))

        def f(*names: str) -> Optional[float]:
            return number(pick(flat, names))

        active_values: List[bool] = []
        for active_name in (
                "data_refreshing", "sync_active", "refresh_active",
                "sync.running", "sync_visual_active"):
            raw_active = pick(flat, (active_name,))
            if isinstance(raw_active, bool):
                active_values.append(raw_active)
            elif isinstance(raw_active, (int, float)):
                active_values.append(bool(raw_active))
            elif isinstance(raw_active, str):
                active_values.append(
                    raw_active.lower() in ("true", "active", "running", "1"))
        active: Optional[bool] = any(active_values) if active_values else None
        boot = pick(flat, ("boot_id", "boot_count", "reset_counter"))
        return cls(
            taken=time.monotonic(),
            uptime=f("uptime_seconds", "uptime_s", "uptime"),
            boot_id=str(boot) if boot is not None else None,
            render_frames=n("render_frames", "frame_count"),
            free_heap=n("free_heap", "free_heap_bytes", "heap_free_bytes",
                        "memory.free_bytes", "heap.free_bytes"),
            largest_block=n("largest_heap_block", "largest_heap_block_bytes",
                            "largest_free_block", "largest_block_bytes"),
            minimum_heap=n("minimum_free_heap", "minimum_free_heap_bytes",
                           "min_free_heap", "heap_low_watermark"),
            spiffs_used=n("spiffs_used_bytes", "filesystem_used_bytes", "storage.used_bytes"),
            spiffs_total=n("spiffs_total_bytes", "filesystem_total_bytes", "storage.total_bytes"),
            cache_bytes=n("icon_cache_bytes", "cache_bytes", "lametric_cache_bytes"),
            cache_count=n("lametric_icons_cached", "icon_cache_count", "cache_file_count"),
            rss_count=n("rss_headline_count", "headline_count", "rss_count",
                        "headlines_count"),
            market_count=n("market_count", "market_item_count", "quotes_count"),
            energy_count=n("energy_point_count", "energy_points", "energy_count"),
            calendar_count=n("calendar_event_count", "calendar_count", "events_count"),
            travel_count=n("travel_count", "travel_stop_count", "departures_stop_count"),
            reddit_count=n("reddit_post_count", "reddit_count", "posts_count"),
            online_store_bytes=n("online_store_bytes", "data_store_bytes", "store_bytes"),
            sync_failures=n("sync_failure_count", "sync_failures", "refresh_failures"),
            tls_contention=n("tls_contention_count", "tls_lease_failures", "tls_busy_count"),
            button_drops=n("button_dropped_events", "button_drop_count", "input_drops"),
            sync_active=active,
            sync_cycles_completed=n("sync_cycles_completed", "refresh_cycles_completed"),
        )

    @property
    def spiffs_free(self) -> Optional[int]:
        if self.spiffs_used is None or self.spiffs_total is None:
            return None
        return self.spiffs_total - self.spiffs_used


@dataclass
class RequestStats:
    lock: threading.Lock = field(default_factory=threading.Lock)
    latencies_ms: List[float] = field(default_factory=list)
    errors: List[str] = field(default_factory=list)

    def success(self, elapsed_ms: float) -> None:
        with self.lock:
            self.latencies_ms.append(elapsed_ms)

    def failure(self, message: str) -> None:
        with self.lock:
            self.errors.append(message)

    def snapshot(self) -> Tuple[List[float], List[str]]:
        with self.lock:
            return list(self.latencies_ms), list(self.errors)


class PageStressor:
    def __init__(self, base: str, timeout: float, interval: float) -> None:
        self.base = base
        self.timeout = timeout
        self.interval = interval
        self.stats = RequestStats()
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, name="page-stress", daemon=True)

    def start(self) -> None:
        self.thread.start()

    def stop(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=self.timeout + 1.0)

    def _run(self) -> None:
        # Current builds hold network work briefly after serving the full page.
        # A long interval exposes starvation without guaranteeing it.
        while not self.stop_event.is_set():
            started = time.monotonic()
            try:
                request(self.base, "/", timeout=self.timeout, expect_json=False)
                self.stats.success((time.monotonic() - started) * 1000.0)
            except RequestFailure as exc:
                self.stats.failure(str(exc))
            self.stop_event.wait(self.interval)


def percentile(values: Sequence[float], fraction: float) -> Optional[float]:
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def format_bytes(value: Optional[int]) -> str:
    return "n/a" if value is None else f"{value / 1024.0:.1f} KiB"


def rebooted(previous: Snapshot, current: Snapshot) -> bool:
    if (previous.boot_id is not None and current.boot_id is not None
            and previous.boot_id != current.boot_id):
        return True
    if (previous.uptime is not None and current.uptime is not None
            and current.uptime + 2.0 < previous.uptime):
        return True
    return (previous.render_frames is not None and current.render_frames is not None
            and current.render_frames < previous.render_frames)


def probe_buttons(base: str, timeout: float, stats: RequestStats) -> None:
    # previous + auto restores automatic mode and approximately restores position.
    for name in ("previous", "auto"):
        started = time.monotonic()
        try:
            request(base, "/api/v1/button", payload={"button": name}, timeout=timeout)
            stats.success((time.monotonic() - started) * 1000.0)
        except RequestFailure as exc:
            stats.failure(str(exc))


def wait_for_sync(client: TelemetryClient, timeout: float, poll_interval: float,
                  previous: Snapshot) -> Tuple[Optional[Snapshot], Optional[str]]:
    deadline = time.monotonic() + timeout
    saw_active = False
    last = previous
    consecutive_idle = 0
    while time.monotonic() < deadline:
        try:
            # The status flag covers the provider and icon phases; resources
            # supplies a monotonic completed-cycle counter so an ignored
            # refresh request cannot be mistaken for an instant success.
            sample = client.collect(include_resources=True)
        except RequestFailure:
            time.sleep(poll_interval)
            continue
        if rebooted(previous, sample):
            return sample, "device rebooted during synchronisation"
        last = sample
        if sample.sync_active is True:
            saw_active = True
            consecutive_idle = 0
        elif sample.sync_active is False:
            consecutive_idle += 1
            if saw_active or consecutive_idle >= 2:
                if (previous.sync_cycles_completed is not None and
                        sample.sync_cycles_completed is not None and
                        sample.sync_cycles_completed <=
                        previous.sync_cycles_completed):
                    return sample, "refresh was not accepted (cycle counter did not advance)"
                return sample, None
        else:
            return sample, "sync-active telemetry is unavailable"
        time.sleep(poll_interval)
    return last, f"synchronisation exceeded {timeout:.0f}s"


def median_snapshot(client: TelemetryClient, samples: int, interval: float) -> Snapshot:
    readings: List[Snapshot] = []
    for index in range(samples):
        readings.append(client.collect())
        if index + 1 < samples:
            time.sleep(interval)
    result = readings[-1]
    for attr in ("free_heap", "largest_block", "minimum_heap", "spiffs_used",
                 "spiffs_total", "cache_bytes", "cache_count", "rss_count",
                 "market_count", "energy_count", "calendar_count", "travel_count",
                 "reddit_count",
                 "online_store_bytes", "sync_failures", "tls_contention",
                 "button_drops", "sync_cycles_completed"):
        values = [getattr(item, attr) for item in readings
                  if getattr(item, attr) is not None]
        if values:
            setattr(result, attr, int(statistics.median(values)))
    return result


def parse_config_limits(base: str, timeout: float) -> Dict[str, int]:
    try:
        config = request(base, "/api/v1/config", timeout=timeout)
    except RequestFailure:
        return {}
    limits: Dict[str, int] = {}
    limits["energy_count"] = 208
    for config_key, snapshot_key in (
        ("rss_headline_count", "rss_count"),
        ("calendar_maximum_events", "calendar_count"),
        ("reddit_post_count", "reddit_count"),
    ):
        parsed = integer(config.get(config_key))
        if parsed is not None:
            limits[snapshot_key] = parsed
    market = config.get("market_items")
    if isinstance(market, list):
        limits["market_count"] = sum(1 for item in market if item)
    elif isinstance(market, str):
        limits["market_count"] = sum(1 for line in market.splitlines()
                                     if line.split("\t", 1)[0].strip())
    travel = config.get("travel_stop_items")
    if isinstance(travel, list):
        limits["travel_count"] = min(16, sum(1 for item in travel if item))
    elif isinstance(travel, str):
        limits["travel_count"] = min(
            16, sum(1 for line in travel.splitlines()
                    if len(line.split("\t", 2)) >= 2
                    and line.split("\t", 2)[1].strip()))
    return limits


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="PikOS host name, IP address, or base URL")
    parser.add_argument("--cycles", type=int, default=5)
    parser.add_argument("--sync-timeout", type=float, default=300.0)
    parser.add_argument("--request-timeout", type=float, default=8.0)
    parser.add_argument("--poll-interval", type=float, default=1.0)
    parser.add_argument("--settle-seconds", type=float, default=4.0)
    parser.add_argument("--maximum-heap-drift", type=int, default=8192)
    parser.add_argument("--maximum-largest-block-drift", type=int, default=8192)
    parser.add_argument("--minimum-free-heap", type=int, default=32768)
    parser.add_argument("--minimum-largest-block", type=int, default=16384)
    parser.add_argument("--minimum-heap-low-water", type=int, default=12288)
    parser.add_argument("--minimum-spiffs-free", type=int, default=32768)
    parser.add_argument("--maximum-spiffs-drift", type=int, default=8192)
    parser.add_argument("--maximum-cache-growth", type=int, default=0)
    parser.add_argument("--maximum-store-drift", type=int, default=4096)
    parser.add_argument("--maximum-count-growth", type=int, default=0)
    parser.add_argument("--maximum-button-p95-ms", type=float, default=250.0)
    parser.add_argument("--maximum-button-ms", type=float, default=750.0)
    parser.add_argument("--minimum-page-success", type=float, default=90.0)
    parser.add_argument("--no-page-stress", action="store_true")
    parser.add_argument("--page-interval", type=float, default=15.0)
    parser.add_argument("--no-button-stress", action="store_true")
    args = parser.parse_args()
    if args.cycles < 1:
        parser.error("--cycles must be at least 1")

    base = normalise_base(args.host)
    client = TelemetryClient(base, args.request_timeout)
    failures: List[str] = []
    warnings: List[str] = []
    button_stats = RequestStats()
    page = None if args.no_page_stress else PageStressor(
        base, args.request_timeout, args.page_interval)

    print(f"PikOS forced-sync stress: {base} ({args.cycles} cycles)")
    try:
        initial = median_snapshot(client, 3, 0.25)
    except RequestFailure as exc:
        print(f"FAIL: cannot collect initial device status: {exc}", file=sys.stderr)
        return 2
    limits = parse_config_limits(base, args.request_timeout)
    print("Initial: heap={} largest={} SPIFFS free={} cache={} store={}".format(
        format_bytes(initial.free_heap), format_bytes(initial.largest_block),
        format_bytes(initial.spiffs_free),
        initial.cache_count if initial.cache_count is not None else "n/a",
        format_bytes(initial.online_store_bytes)))

    if page:
        page.start()
    snapshots: List[Snapshot] = []
    previous = initial
    try:
        if not args.no_button_stress:
            probe_buttons(base, args.request_timeout, button_stats)
        for cycle in range(1, args.cycles + 1):
            started = time.monotonic()
            try:
                request(base, "/api/v1/refresh", payload={}, timeout=args.request_timeout)
            except RequestFailure as exc:
                failures.append(f"cycle {cycle}: refresh request failed: {exc}")
                print(f"Cycle {cycle}: FAIL to start ({exc})")
                continue
            final, problem = wait_for_sync(client, args.sync_timeout,
                                           args.poll_interval, previous)
            if problem:
                failures.append(f"cycle {cycle}: {problem}")
                print(f"Cycle {cycle}: FAIL ({problem})")
                if final is not None:
                    previous = final
                continue
            time.sleep(args.settle_seconds)
            try:
                settled = median_snapshot(client, 3, 0.25)
            except RequestFailure as exc:
                failures.append(f"cycle {cycle}: post-sync telemetry failed: {exc}")
                continue
            if rebooted(previous, settled):
                failures.append(f"cycle {cycle}: device rebooted")
            snapshots.append(settled)
            previous = settled
            elapsed = time.monotonic() - started
            print("Cycle {}: {:.1f}s, heap={}, largest={}, SPIFFS free={}, "
                  "cache={}, counts={}/{}/{}/{}/{}/{}".format(
                      cycle, elapsed, format_bytes(settled.free_heap),
                      format_bytes(settled.largest_block), format_bytes(settled.spiffs_free),
                      settled.cache_count if settled.cache_count is not None else "n/a",
                      settled.rss_count if settled.rss_count is not None else "-",
                      settled.market_count if settled.market_count is not None else "-",
                      settled.energy_count if settled.energy_count is not None else "-",
                      settled.calendar_count if settled.calendar_count is not None else "-",
                      settled.travel_count if settled.travel_count is not None else "-",
                      settled.reddit_count if settled.reddit_count is not None else "-"))
            for attr, maximum in limits.items():
                observed = getattr(settled, attr)
                if observed is not None and maximum >= 0 and observed > maximum:
                    failures.append(
                        f"cycle {cycle}: {attr}={observed} exceeds configured cap {maximum}")
            if settled.spiffs_free is not None and settled.spiffs_free < args.minimum_spiffs_free:
                failures.append(
                    f"cycle {cycle}: SPIFFS free {settled.spiffs_free} B is below "
                    f"{args.minimum_spiffs_free} B")
            if settled.free_heap is not None and settled.free_heap < args.minimum_free_heap:
                failures.append(
                    f"cycle {cycle}: free heap {settled.free_heap} B is below "
                    f"{args.minimum_free_heap} B")
            if (settled.largest_block is not None
                    and settled.largest_block < args.minimum_largest_block):
                failures.append(
                    f"cycle {cycle}: largest free block {settled.largest_block} B is below "
                    f"{args.minimum_largest_block} B")
            if (settled.minimum_heap is not None
                    and settled.minimum_heap < args.minimum_heap_low_water):
                failures.append(
                    f"cycle {cycle}: heap low-water mark {settled.minimum_heap} B is below "
                    f"{args.minimum_heap_low_water} B")
            if not args.no_button_stress:
                probe_buttons(base, args.request_timeout, button_stats)
    finally:
        if page:
            page.stop()

    if snapshots:
        warm, final = snapshots[0], snapshots[-1]
        if len(snapshots) < 2:
            warnings.append("only the warm-up cycle completed; drift checks need two cycles")
        for attr, allowance, label in (
            ("free_heap", args.maximum_heap_drift, "free heap"),
            ("largest_block", args.maximum_largest_block_drift, "largest free block"),
        ):
            before = getattr(warm, attr)
            observed = [getattr(sample, attr) for sample in snapshots]
            observed = [value for value in observed if value is not None]
            if before is None or not observed:
                warnings.append(f"{label} drift check skipped (field unavailable)")
            elif before - min(observed) > allowance:
                failures.append(
                    f"{label} fell {before - min(observed)} B below warm-up")
        if warm.spiffs_used is None or final.spiffs_used is None:
            warnings.append("SPIFFS drift check skipped (resource telemetry unavailable)")
        else:
            peak = max(sample.spiffs_used for sample in snapshots
                       if sample.spiffs_used is not None)
            if peak - warm.spiffs_used > args.maximum_spiffs_drift:
                failures.append(f"SPIFFS usage grew {peak - warm.spiffs_used} B after warm-up")
        if warm.cache_count is None or final.cache_count is None:
            warnings.append("cache-count drift check skipped (field unavailable)")
        else:
            peak = max(sample.cache_count for sample in snapshots
                       if sample.cache_count is not None)
            if peak - warm.cache_count > args.maximum_cache_growth:
                failures.append(f"cache grew by {peak - warm.cache_count} files after warm-up")
        if warm.online_store_bytes is None or final.online_store_bytes is None:
            warnings.append("online-store drift check skipped (field unavailable)")
        else:
            peak = max(sample.online_store_bytes for sample in snapshots
                       if sample.online_store_bytes is not None)
            if peak - warm.online_store_bytes > args.maximum_store_drift:
                failures.append(f"online store grew {peak - warm.online_store_bytes} B after warm-up")
        for attr, label in (("sync_failures", "sync failures"),
                            ("tls_contention", "TLS contention"),
                            ("button_drops", "button drops")):
            before, after = getattr(warm, attr), getattr(final, attr)
            if before is not None and after is not None and after > before:
                failures.append(f"{label} increased by {after - before}")
        for attr, label in (("rss_count", "RSS count"),
                            ("market_count", "market count"),
                            ("energy_count", "energy point count"),
                            ("calendar_count", "Calendar count"),
                            ("travel_count", "Travel stop count"),
                            ("reddit_count", "Reddit count")):
            baseline = getattr(warm, attr)
            observed = [getattr(sample, attr) for sample in snapshots]
            observed = [value for value in observed if value is not None]
            if baseline is not None and observed:
                growth = max(observed) - baseline
                if growth > args.maximum_count_growth:
                    failures.append(f"{label} grew by {growth} items after warm-up")
    else:
        failures.append("no forced synchronisation cycle completed")

    button_latencies, button_errors = button_stats.snapshot()
    if not args.no_button_stress:
        p95 = percentile(button_latencies, 0.95)
        maximum = max(button_latencies) if button_latencies else None
        print("Button API: {} successful, {} failed, p95={}, max={}".format(
            len(button_latencies), len(button_errors),
            "n/a" if p95 is None else f"{p95:.0f} ms",
            "n/a" if maximum is None else f"{maximum:.0f} ms"))
        if button_errors:
            failures.append(f"{len(button_errors)} button API requests failed")
        if p95 is not None and p95 > args.maximum_button_p95_ms:
            failures.append(f"button API p95 {p95:.0f} ms exceeds {args.maximum_button_p95_ms:.0f} ms")
        if maximum is not None and maximum > args.maximum_button_ms:
            failures.append(f"button API max {maximum:.0f} ms exceeds {args.maximum_button_ms:.0f} ms")

    if page:
        page_latencies, page_errors = page.stats.snapshot()
        total = len(page_latencies) + len(page_errors)
        success = 100.0 * len(page_latencies) / total if total else 0.0
        print(f"Settings page: {len(page_latencies)}/{total} successful ({success:.1f}%)")
        if total == 0:
            warnings.append("settings-page stress made no requests")
        elif success < args.minimum_page_success:
            failures.append(
                f"settings-page success {success:.1f}% is below {args.minimum_page_success:.1f}%")

    warnings.extend(client.notices)
    for message in dict.fromkeys(warnings):
        print(f"SKIP/WARN: {message}")
    if failures:
        for message in dict.fromkeys(failures):
            print(f"FAIL: {message}")
        return 1
    print("PASS: sequential syncs completed without detected reboot, drift, or responsiveness regression")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
