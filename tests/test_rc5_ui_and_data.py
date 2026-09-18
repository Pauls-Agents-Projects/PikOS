"""RC5 contracts for timelines, calendar streaming and app windows."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class Rc5UiAndDataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.web = (ROOT / "include" / "WebUi.h").read_text(encoding="utf-8")
        cls.calendar = (ROOT / "src" / "DataServices.cpp").read_text(
            encoding="utf-8"
        )
        cls.peripherals = (ROOT / "src" / "Peripherals.cpp").read_text(
            encoding="utf-8"
        )
        cls.portal = (ROOT / "src" / "WebPortal.cpp").read_text(encoding="utf-8")

    def test_battery_timeline_uses_real_sample_ages(self) -> None:
        self.assertIn("snapshot.ageSeconds[index]", self.peripherals)
        self.assertIn('createNestedArray("age_seconds")', self.portal)
        self.assertIn("payload.age_seconds", self.web)
        self.assertIn("Intl.DateTimeFormat", self.web)

    def test_calendar_does_not_read_from_an_empty_keep_alive_socket(self) -> None:
        fetch = self.calendar.split("bool DataServices::fetchCalendarUrl", 1)[1]
        fetch = fetch.split("bool DataServices::fetchTravel", 1)[0]
        self.assertIn("const int contentLength = http.getSize();", fetch)
        self.assertIn("if (stream->available() <= 0)", fetch)
        self.assertIn("contentLength < 0 || total <", fetch)

    def test_apps_are_horizontal_minimizable_windows(self) -> None:
        self.assertIn("scroll-snap-type:x mandatory", self.web)
        self.assertIn('class="app-rail"', self.web)
        self.assertIn('class="app-toggle"', self.web)
        self.assertIn("function setAppWindowEnabled", self.web)
        self.assertIn('.app-box.disabled>:not(h3)', self.web)

    def test_global_motion_and_sync_controls_live_under_device(self) -> None:
        sync = self.web.index('id="sync_every_loops"')
        apps = self.web.index('data-section="apps"')
        motion = self.web.index('data-ui="motionSettings"')
        self.assertLess(sync, apps)
        self.assertLess(motion, apps)
        for control in (
            "app_duration_seconds",
            "maximum_app_seconds",
            "scroll_step_ms",
            "maximum_message_chars",
        ):
            self.assertEqual(self.web.count(f'id="{control}"'), 1)


if __name__ == "__main__":
    unittest.main()
