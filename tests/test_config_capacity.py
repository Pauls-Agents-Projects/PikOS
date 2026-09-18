"""Reproducible upper-bound fixture for PikOS's persisted dynamic settings.

This is intentionally a conservative host estimate, not a second firmware
serializer: ArduinoJson's fixed writer remains the authority on-device. The
historical schema-24 worst-case measurement was 15,519 bytes. The test adds
the exact compact-JSON delta of all schema-25 Travel fields, while also keeping
one readable fixture containing every current repeatable/dynamic collection.
"""

from __future__ import annotations

import json
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
PERSISTENCE_CAPACITY = 20 * 1024
SETTINGS_REQUEST_CAPACITY = 16 * 1024
HISTORICAL_SCHEMA_24_BOUNDARY_BYTES = 15_519
EXPECTED_TRAVEL_DELTA_BYTES = 1_089
EXPECTED_SCHEMA_25_BOUNDARY_BYTES = 16_608
EXPECTED_DYNAMIC_COLLECTION_FIXTURE_BYTES = 9_321
EXPECTED_MAXIMUM_GARMIN_PATCH_BYTES = 3_455


def compact_size(value: object) -> int:
    return len(
        json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode(
            "utf-8"
        )
    )


def max_travel_rows() -> str:
    rows: list[str] = []
    for index in range(16):
        # Firmware normalization caps these fields at 24/12/8 characters.
        label = f"L{index:02d}" + "X" * 21
        stop_id = f"S{index:02d}" + "Y" * 9
        line = f"R{index:02d}" + "Z" * 5
        rows.append(f"{label}\t{stop_id}\t{line}\t4294967295")
    return "\n".join(rows)


def maximum_dynamic_collections() -> dict[str, object]:
    alarm = "23:59\t127\t" + "A" * 48 + "\tcustom6\t1"
    rtttl = "Tune:d=32,o=7,b=255:" + "c," * 400
    rtttl = rtttl[:768]
    return {
        "clock_timezone_items": "\n".join(
            f"{zone}\t4294967295"
            for zone in (
                "local",
                "los_angeles",
                "new_york",
                "sao_paulo",
                "johannesburg",
                "kathmandu",
                "adelaide",
                "auckland",
            )
        ),
        "alarm_items": ("\n".join([alarm] * 12))[:1536],
        "rss_feed_items": "\n".join(
            f"https://feed{index}.example/" + "article/" * 16 +
            "\t4294967295"
            for index in range(6)
        ),
        "market_items": "\n".join(
            f"MAXIMUM-TICKER-{index}\t4294967295" for index in range(6)
        ),
        # Persistence redacts each calendar credential to its fixed vault slot.
        "calendar_items": "\n".join(
            f"{'C' * 22}{index:02d}\t@{index}\t4294967295"
            for index in range(16)
        ),
        "travel_stop_items": max_travel_rows(),
        "reddit_items": "\n".join(
            f"{'r' * 19}{index:02d}\t4294967295" for index in range(16)
        ),
        **{f"custom_rtttl_{index}": rtttl for index in range(1, 7)},
    }


class ConfigCapacityTests(unittest.TestCase):
    def test_all_dynamic_collections_fit_the_new_scratch_ceiling(self) -> None:
        fixture = maximum_dynamic_collections()
        self.assertEqual(len(fixture["travel_stop_items"].splitlines()), 16)
        measured = compact_size(fixture)
        self.assertEqual(measured, EXPECTED_DYNAMIC_COLLECTION_FIXTURE_BYTES)
        self.assertLess(measured, PERSISTENCE_CAPACITY)

    def test_historical_full_config_plus_travel_has_headroom(self) -> None:
        sentinel = {"sentinel": True}
        with_travel = {
            **sentinel,
            "travel_enabled": False,
            "travel_animation": "ticker",
            "travel_color": "#64d2ff",
            "travel_stop_items": max_travel_rows(),
        }
        travel_delta = compact_size(with_travel) - compact_size(sentinel)
        estimate = HISTORICAL_SCHEMA_24_BOUNDARY_BYTES + travel_delta
        self.assertEqual(travel_delta, EXPECTED_TRAVEL_DELTA_BYTES)
        self.assertEqual(estimate, EXPECTED_SCHEMA_25_BOUNDARY_BYTES)
        self.assertGreater(estimate, 16 * 1024)
        self.assertLess(estimate, PERSISTENCE_CAPACITY)

    def test_firmware_capacity_matches_the_boundary_contract(self) -> None:
        source = (ROOT / "src" / "Config.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "constexpr size_t kPersistenceCapacity = 20U * 1024U;", source
        )

    def test_maximum_garmin_patch_stays_bounded_for_zero_copy_parse(self) -> None:
        garmin_patch = {
            "garmin_domain": "garmin.com",
            "garmin_client_id": "C" * 128,
            "garmin_refresh_token": "R" * 3072,
            "garmin_display_name": "D" * 128,
            "activity_enabled": True,
        }
        patch_size = compact_size(garmin_patch)
        self.assertEqual(patch_size, EXPECTED_MAXIMUM_GARMIN_PATCH_BYTES)
        # handleSaveConfig() parses a mutable request buffer in-place. The
        # 3 KiB refresh-token value therefore adds one variant slot, not 3 KiB
        # to ArduinoJson's fixed arena.
        self.assertLess(patch_size, 4096)

    def test_web_request_arena_covers_flat_field_slots(self) -> None:
        source = (ROOT / "include" / "WebPortal.h").read_text(
            encoding="utf-8"
        )
        self.assertIn("StaticJsonDocument<16384> jsonDocument_;", source)
        config_source = (ROOT / "src" / "Config.cpp").read_text(
            encoding="utf-8"
        )
        writer = config_source.split("void writeConfigJson", 1)[1].split(
            "bool readSecretVault", 1
        )[0]
        # Count every flat assignment conservatively, including duplicates in
        # branches. Sixty-four bytes per field is four times ArduinoJson's
        # 32-bit VariantSlot size and still leaves the fixed arena headroom.
        field_assignments = writer.count('root["')
        self.assertGreater(field_assignments, 100)
        self.assertLess(field_assignments * 64, SETTINGS_REQUEST_CAPACITY)


if __name__ == "__main__":
    unittest.main()
