#!/usr/bin/env python3
"""Send a short test animation to PikOS's authenticated HTTP canvas."""

from __future__ import annotations

import argparse
import colorsys
import http.client
import json
import math
import time

WIDTH = 32
HEIGHT = 8


def make_frame(pattern: str, phase: float,
               color: tuple[int, int, int]) -> list[int]:
    frame: list[int] = []
    for y in range(HEIGHT):
        for x in range(WIDTH):
            if pattern == "solid":
                rgb = color
            elif pattern == "checker":
                rgb = color if ((x // 2 + y // 2) & 1) == 0 else (0, 0, 0)
            elif pattern == "plasma":
                value = (math.sin(x * 0.34 + phase * 4)
                         + math.sin(y * 0.72 - phase * 3)
                         + math.sin((x + y) * 0.22 + phase * 2))
                hue = (value / 6.0 + phase) % 1.0
                rgb = tuple(round(channel * 255) for channel in
                            colorsys.hsv_to_rgb(hue, 1, 0.7))
            else:
                hue = (x / WIDTH + y / HEIGHT * 0.15 + phase) % 1.0
                rgb = tuple(round(channel * 255) for channel in
                            colorsys.hsv_to_rgb(hue, 1, 0.65))
            frame.append((rgb[0] << 16) | (rgb[1] << 8) | rgb[2])
    return frame


def parse_color(value: str) -> tuple[int, int, int]:
    value = value.removeprefix("#")
    if len(value) != 6:
        raise argparse.ArgumentTypeError("color must be RRGGBB or #RRGGBB")
    try:
        return tuple(int(value[index:index + 2], 16) for index in (0, 2, 4))
    except ValueError as exc:
        raise argparse.ArgumentTypeError("invalid hexadecimal color") from exc


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--ip", required=True, help="PikOS IP address")
    parser.add_argument("--api-key", required=True,
                        help="API key shown in the settings JSON")
    parser.add_argument("--pattern",
                        choices=("rainbow", "plasma", "checker", "solid"),
                        default="rainbow")
    parser.add_argument("--color", type=parse_color, default=(101, 255, 182))
    parser.add_argument("--fps", type=float, default=4,
                        help="HTTP canvas rate, limited to 10 fps")
    parser.add_argument("--seconds", type=float, default=10)
    args = parser.parse_args()

    frame_period = 1.0 / max(1.0, min(args.fps, 10.0))
    started = time.monotonic()
    next_frame = started
    connection = http.client.HTTPConnection(args.ip, 80, timeout=3)
    while time.monotonic() - started < args.seconds:
        now = time.monotonic()
        body = json.dumps({"pixels": make_frame(
            args.pattern, (now - started) * 0.12, args.color)})
        connection.request("POST", "/api/v1/frame", body,
                           {"Content-Type": "application/json",
                            "X-Pixel-Key": args.api_key})
        response = connection.getresponse()
        response.read()
        if response.status != 200:
            raise SystemExit(f"PikOS returned HTTP {response.status}")
        next_frame += frame_period
        time.sleep(max(0.0, next_frame - time.monotonic()))


if __name__ == "__main__":
    main()
