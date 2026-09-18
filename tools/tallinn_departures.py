"""Bounded reference parser for Tallinn's undocumented stop-departure CSV.

This host-side implementation defines and tests the limits intended for the
future PikOS provider. It performs no network requests and has no dependencies.
"""

from __future__ import annotations

from dataclasses import dataclass
import re


MAX_BODY_BYTES = 8192
MAX_LINES = 64
MAX_LINE_BYTES = 160
MAX_PARSED_ROWS = 16
MAX_RESULTS = 4
EXPECTED_HEADER = (
    "Transport",
    "RouteNum",
    "ExpectedTimeInSeconds",
    "ScheduleTimeInSeconds",
)
SAFE_STOP = re.compile(r"[A-Za-z0-9._-]{1,12}\Z")
SAFE_LINE = re.compile(r"[A-Za-z0-9+._-]{1,8}\Z")
SAFE_TRANSPORT = re.compile(r"[A-Za-z]{1,16}\Z")


class DepartureFormatError(ValueError):
    """The response is unsafe, inconsistent, or not the expected format."""


@dataclass(frozen=True)
class Departure:
    transport: str
    route: str
    destination: str
    departure_epoch: int
    scheduled_epoch: int
    realtime: bool


@dataclass(frozen=True)
class ParseResult:
    departures: tuple[Departure, ...]
    accepted_rows: int


def _integer(value: str, name: str, low: int, high: int) -> int:
    if not value.isascii() or not value.isdecimal():
        raise DepartureFormatError(f"invalid {name}")
    parsed = int(value)
    if parsed < low or parsed > high:
        raise DepartureFormatError(f"out-of-range {name}")
    return parsed


def _contains_control(value: str) -> bool:
    return any(ord(character) < 0x20 or ord(character) == 0x7F for character in value)


def _scheduled_offset(expected: int, scheduled: int) -> int:
    offset = scheduled - expected
    while offset <= -43200:
        offset += 86400
    while offset > 43200:
        offset -= 86400
    return offset


def parse_departures(
    body: bytes | str,
    requested_stop: str,
    fetched_at: int,
    line_filter: str = "",
) -> ParseResult:
    if not SAFE_STOP.fullmatch(requested_stop):
        raise DepartureFormatError("invalid requested stop")
    if line_filter and not SAFE_LINE.fullmatch(line_filter):
        raise DepartureFormatError("invalid line filter")
    if not isinstance(fetched_at, int) or isinstance(fetched_at, bool) or fetched_at < 0:
        raise DepartureFormatError("invalid fetch time")

    raw = body.encode("utf-8") if isinstance(body, str) else body
    if not raw or len(raw) > MAX_BODY_BYTES:
        raise DepartureFormatError("empty or oversized body")
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise DepartureFormatError("body is not UTF-8") from exc
    lines = text.splitlines()
    if not lines or len(lines) > MAX_LINES:
        raise DepartureFormatError("invalid line count")
    for line in lines:
        if len(line.encode("utf-8")) > MAX_LINE_BYTES or _contains_control(line):
            raise DepartureFormatError("invalid line")

    header = lines[0].strip().split(",")
    if len(header) < 6 or tuple(header[:4]) != EXPECTED_HEADER:
        raise DepartureFormatError("unknown response header")
    server_now = _integer(header[4], "server time", 0, 86399)

    in_requested_block = False
    saw_requested_block = False
    valid_rows = 0
    candidates: list[Departure] = []

    for raw_line in lines[1:]:
        line = raw_line.strip()
        if not line:
            continue
        fields = line.split(",")
        if fields[0] == "stop":
            if len(fields) != 2 or not SAFE_STOP.fullmatch(fields[1]):
                raise DepartureFormatError("invalid stop row")
            in_requested_block = fields[1] == requested_stop
            saw_requested_block = saw_requested_block or in_requested_block
            continue
        if not in_requested_block:
            continue
        if len(fields) not in {6, 7}:
            raise DepartureFormatError("invalid departure field count")

        transport, route, expected_text, scheduled_text, destination, wait_text = fields[:6]
        if not SAFE_TRANSPORT.fullmatch(transport) or not SAFE_LINE.fullmatch(route):
            raise DepartureFormatError("invalid transport or route")
        if not destination or len(destination.encode("utf-8")) > 64 or _contains_control(destination):
            raise DepartureFormatError("invalid destination")
        expected = _integer(expected_text, "expected time", 0, 172799)
        scheduled = _integer(scheduled_text, "scheduled time", 0, 172799)
        wait = _integer(wait_text, "wait", 0, 86400)
        if len(fields) == 7 and fields[6] not in {"", "Z"}:
            raise DepartureFormatError("unknown departure flag")

        wrapped_delta = (expected % 86400 - server_now) % 86400
        discrepancy = min(abs(wait - wrapped_delta), abs(wait - (wrapped_delta + 86400)))
        if discrepancy > 120:
            raise DepartureFormatError("wait and expected time disagree")

        valid_rows += 1
        if valid_rows > MAX_PARSED_ROWS:
            continue
        if line_filter and route.casefold() != line_filter.casefold():
            continue
        departure_epoch = fetched_at + wait
        candidates.append(
            Departure(
                transport=transport,
                route=route,
                destination=destination,
                departure_epoch=departure_epoch,
                scheduled_epoch=departure_epoch + _scheduled_offset(expected, scheduled),
                realtime=expected != scheduled,
            )
        )

    if not saw_requested_block:
        raise DepartureFormatError("requested stop block is missing")

    candidates.sort(key=lambda item: (item.departure_epoch, item.route.casefold()))
    unique: list[Departure] = []
    seen: set[tuple[str, int]] = set()
    for candidate in candidates:
        key = candidate.route.casefold(), candidate.departure_epoch
        if key in seen:
            continue
        seen.add(key)
        unique.append(candidate)
        if len(unique) == MAX_RESULTS:
            break
    return ParseResult(tuple(unique), min(valid_rows, MAX_PARSED_ROWS))
