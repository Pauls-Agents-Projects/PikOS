#!/usr/bin/env python3
"""Watch real PikOS LED frames for transient graphical corruption.

The test reads the same RGB565 framebuffer that backs the browser twin. It
detects malformed/torn responses, one-frame blank flashes, isolated frames
that disagree with both neighbours, long freezes, and discontinuous ticker
motion. Suspicious frame triplets are saved as enlarged PPM images.

Example:
  python3 tools/monitor_visual_integrity.py pikos.local --app rss --seconds 30
"""

from __future__ import annotations

import argparse
import json
import pathlib
import time
import urllib.error
import urllib.request
from dataclasses import dataclass

from pikos_session import open_pikos


WIDTH = 32
HEIGHT = 8
PIXELS = WIDTH * HEIGHT


@dataclass
class Frame:
    captured_at: float
    pixels: tuple[int, ...]

    @property
    def lit(self) -> int:
        return sum(pixel != 0 for pixel in self.pixels)


def request(url: str, *, body: dict | None = None, timeout: float = 2.0) -> bytes:
    data = None if body is None else json.dumps(body).encode()
    headers = {} if body is None else {"Content-Type": "application/json"}
    method = "GET" if body is None else "POST"
    with open_pikos(
        urllib.request.Request(url, data=data, headers=headers, method=method),
        timeout=timeout,
    ) as response:
        return response.read()


def status(base_url: str) -> dict:
    return json.loads(request(base_url + "/api/v1/status"))


def capture(base_url: str) -> Frame | None:
    payload = request(base_url + "/api/v1/display?format=rgb565")
    if not payload:  # Expected 204 while a provider owns the radio.
        return None
    if len(payload) != 1 + PIXELS * 2:
        raise ValueError(f"malformed framebuffer: {len(payload)} bytes")
    pixels = tuple(
        (payload[offset] << 8) | payload[offset + 1]
        for offset in range(1, len(payload), 2)
    )
    return Frame(time.monotonic(), pixels)


def distance(left: Frame, right: Frame) -> int:
    return sum(a != b for a, b in zip(left.pixels, right.pixels))


def mask(frame: Frame) -> tuple[bool, ...]:
    # Ignore the low-intensity slideshow marker on the bottom row. Its progress
    # is independent of the foreground animation and would look like motion.
    return tuple(pixel != 0 and index // WIDTH < HEIGHT - 1
                 for index, pixel in enumerate(frame.pixels))


def ticker_residual(left: Frame, right: Frame) -> float:
    before, after = mask(left), mask(right)
    best = 1.0
    for shift in range(-4, 5):
        mismatch = union = 0
        for y in range(HEIGHT - 1):
            for x in range(WIDTH):
                a = before[y * WIDTH + x]
                source_x = x - shift
                b = (after[y * WIDTH + source_x]
                     if 0 <= source_x < WIDTH else False)
                mismatch += a != b
                union += a or b
        if union:
            best = min(best, mismatch / union)
    return best


def rgb565(pixel: int) -> tuple[int, int, int]:
    return (
        ((pixel >> 11) & 0x1F) * 255 // 31,
        ((pixel >> 5) & 0x3F) * 255 // 63,
        (pixel & 0x1F) * 255 // 31,
    )


def save_triplet(path: pathlib.Path, frames: tuple[Frame, Frame, Frame]) -> None:
    scale = 8
    gap = scale
    width = WIDTH * scale * 3 + gap * 2
    height = HEIGHT * scale
    with path.open("wb") as output:
        output.write(f"P6\n{width} {height}\n255\n".encode())
        for y in range(height):
            row = bytearray()
            for panel, frame in enumerate(frames):
                if panel:
                    row.extend(b"\x20\x20\x20" * gap)
                source_y = y // scale
                for x in range(WIDTH * scale):
                    row.extend(rgb565(frame.pixels[source_y * WIDTH + x // scale]))
            output.write(row)


def select_app(base_url: str, wanted: str) -> None:
    for _ in range(9):
        current = status(base_url).get("current_app")
        if current == wanted:
            return
        request(base_url + "/api/v1/button", body={"button": "next"})
        time.sleep(0.25)
    raise RuntimeError(f"could not select enabled app {wanted!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="PikOS hostname or IP address")
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--interval", type=float, default=0.10)
    parser.add_argument(
        "--app", choices=("rss", "reddit", "calendar"),
        help="Hold this app for a repeatable animated test, then restore Auto",
    )
    parser.add_argument("--maximum-freeze", type=float, default=2.0)
    parser.add_argument("--maximum-discontinuities", type=int, default=12)
    parser.add_argument(
        "--artifacts", type=pathlib.Path,
        default=pathlib.Path("artifacts/visual-glitches"),
    )
    parser.add_argument(
        "--fail-on-network-error", action="store_true",
        help="Also fail when a framebuffer request is lost",
    )
    args = parser.parse_args()

    base_url = (args.host if "://" in args.host else "http://" + args.host).rstrip("/")
    selected = False
    if args.app:
        select_app(base_url, args.app)
        selected = True
        time.sleep(2.0)

    frames: list[Frame] = []
    errors: list[str] = []
    skipped = 0
    deadline = time.monotonic() + args.seconds
    try:
        while time.monotonic() < deadline:
            started = time.monotonic()
            try:
                frame = capture(base_url)
                if frame is None:
                    skipped += 1
                else:
                    frames.append(frame)
            except (OSError, urllib.error.URLError, ValueError) as error:
                errors.append(str(error))
            time.sleep(max(0.0, args.interval - (time.monotonic() - started)))
    finally:
        if selected:
            try:
                request(base_url + "/api/v1/button", body={"button": "auto"})
            except OSError:
                pass

    if len(frames) < 10:
        print(f"FAIL: only {len(frames)} valid frames ({len(errors)} errors)")
        return 2
    if max(frame.lit for frame in frames) <= 1:
        print("FAIL: display remained blank (sleep mode or failed wake)")
        return 2

    blank_flashes: list[int] = []
    isolated: list[int] = []
    discontinuities: list[int] = []
    for index in range(1, len(frames) - 1):
        previous, current, following = frames[index - 1:index + 2]
        outside = distance(previous, following)
        before = distance(previous, current)
        after = distance(current, following)
        if current.lit <= 1 and previous.lit >= 8 and following.lit >= 8:
            blank_flashes.append(index)
        # A corrupt/torn frame commonly differs sharply from both neighbours,
        # while those neighbours agree. Normal animation changes progressively.
        if outside <= 6 and before >= 18 and after >= 18:
            isolated.append(index)
        if args.app in ("rss", "reddit") and ticker_residual(previous, current) > 0.72:
            discontinuities.append(index)

    longest_freeze = 0.0
    freeze_started = frames[0].captured_at
    for previous, current in zip(frames, frames[1:]):
        if previous.pixels != current.pixels:
            freeze_started = current.captured_at
        longest_freeze = max(longest_freeze, current.captured_at - freeze_started)

    excessive_motion = (
        discontinuities if len(discontinuities) > args.maximum_discontinuities
        else []
    )
    suspects = sorted(set(blank_flashes + isolated + excessive_motion))
    if suspects:
        args.artifacts.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S")
        for number, index in enumerate(suspects[:12], 1):
            save_triplet(
                args.artifacts / f"{stamp}-{number:02d}.ppm",
                tuple(frames[index - 1:index + 2]),
            )

    failed = (
        bool(blank_flashes)
        or bool(isolated)
        or len(discontinuities) > args.maximum_discontinuities
        or (args.app is not None and longest_freeze > args.maximum_freeze)
        or (bool(errors) and args.fail_on_network_error)
    )
    print(
        f"{'FAIL' if failed else 'PASS'}: {len(frames)} frames, "
        f"{len(blank_flashes)} blank flash(es), {len(isolated)} isolated "
        f"corruption(s), {len(discontinuities)} ticker discontinuity sample(s), "
        f"longest freeze {longest_freeze:.2f}s, {skipped} busy skip(s), "
        f"{len(errors)} request error(s)"
    )
    if suspects:
        print(f"Saved {min(12, len(suspects))} diagnostic triplet(s) to {args.artifacts}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
