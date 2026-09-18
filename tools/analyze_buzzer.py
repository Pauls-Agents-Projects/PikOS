#!/usr/bin/env python3
"""Measure buzzer note level and pitch from a mono PCM WAV recording."""

from __future__ import annotations

import argparse
import array
import json
import math
import sys
import wave


def goertzel(samples: list[float], sample_rate: int, frequency: float) -> float:
    coefficient = 2.0 * math.cos(2.0 * math.pi * frequency / sample_rate)
    previous = 0.0
    previous_two = 0.0
    for sample in samples:
        current = sample + coefficient * previous - previous_two
        previous_two = previous
        previous = current
    return (
        previous_two * previous_two
        + previous * previous
        - coefficient * previous * previous_two
    )


def measure(
    pcm: array.array[int],
    sample_rate: int,
    start_seconds: float,
    expected_hz: float,
    duration_seconds: float,
) -> dict[str, float]:
    first = max(0, round(start_seconds * sample_rate))
    count = max(1, round(duration_seconds * sample_rate))
    raw = pcm[first : first + count]
    if not raw:
        raise ValueError(f"window at {start_seconds}s is outside the recording")

    mean = sum(raw) / len(raw)
    rms = math.sqrt(sum((sample - mean) ** 2 for sample in raw) / len(raw))
    rms_dbfs = 20.0 * math.log10(max(rms / 32768.0, 1e-12))

    # A Hann window suppresses leakage, while a narrow scan around the RTTTL
    # target avoids choosing a louder square-wave harmonic.
    windowed = [
        (sample - mean)
        * (0.5 - 0.5 * math.cos(2.0 * math.pi * index / (len(raw) - 1)))
        for index, sample in enumerate(raw)
    ]
    minimum = expected_hz * 0.97
    maximum = expected_hz * 1.03
    step = max(0.25, expected_hz / 2500.0)
    best_frequency = minimum
    best_power = -1.0
    frequency = minimum
    while frequency <= maximum:
        power = goertzel(windowed, sample_rate, frequency)
        if power > best_power:
            best_power = power
            best_frequency = frequency
        frequency += step

    cents = 1200.0 * math.log2(best_frequency / expected_hz)
    return {
        "start_seconds": start_seconds,
        "expected_hz": expected_hz,
        "measured_hz": round(best_frequency, 2),
        "pitch_error_cents": round(cents, 2),
        "rms_dbfs": round(rms_dbfs, 2),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wav")
    parser.add_argument(
        "--tone",
        action="append",
        required=True,
        metavar="START_SECONDS:EXPECTED_HZ",
    )
    parser.add_argument("--duration", type=float, default=0.5)
    arguments = parser.parse_args()

    with wave.open(arguments.wav, "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            parser.error("the WAV must be mono 16-bit PCM")
        sample_rate = source.getframerate()
        pcm = array.array("h", source.readframes(source.getnframes()))
    if sys.byteorder == "big":
        pcm.byteswap()

    results = []
    for specification in arguments.tone:
        start, expected = specification.split(":", 1)
        results.append(
            measure(
                pcm,
                sample_rate,
                float(start),
                float(expected),
                arguments.duration,
            )
        )
    print(json.dumps(results, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
