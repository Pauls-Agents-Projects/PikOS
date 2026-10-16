"""Keep the physical and browser travel summaries intentionally aligned."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class TravelDisplayContractTests(unittest.TestCase):
    def test_cache_keeps_four_but_display_limits_itself_to_two(self) -> None:
        header = (ROOT / "include" / "DataServices.h").read_text(encoding="utf-8")
        engine = (ROOT / "src" / "AppEngine.cpp").read_text(encoding="utf-8")
        web = (ROOT / "include" / "WebUi.h").read_text(encoding="utf-8")
        self.assertIn("kMaximumTravelDepartures = 4", header)
        self.assertIn("kDisplayedTravelDepartures = 2", engine)
        self.assertIn("stop.departures.slice(0,4)", web)

    def test_device_shows_route_once_and_second_departure_as_clock_time(self) -> None:
        source = (ROOT / "src" / "AppEngine.cpp").read_text(encoding="utf-8")
        travel = next(
            section
            for section in source.split("case LocalApp::Travel:")[1:]
            if "String routeLabel" in section
        ).split("case LocalApp::Activity:", 1)[0]
        self.assertIn('message = routeLabel + " ";', travel)
        self.assertNotIn("message += departure.route;", travel)
        self.assertIn("if (index == 0)", travel)
        self.assertIn("formattedClockTime(departureTime, config.clock24Hour)", travel)

    def test_browser_does_not_repeat_route_per_departure(self) -> None:
        source = (ROOT / "include" / "WebUi.h").read_text(encoding="utf-8")
        renderer = source.split("function notificationEntries", 1)[1].split(
            "function renderNotificationFeed", 1
        )[0]
        self.assertIn("stop?.line||departures[0]?.route", renderer)
        self.assertIn('lines.join("  ·  ")', renderer)
        self.assertNotIn("departure?.route||stop?.line", renderer)


if __name__ == "__main__":
    unittest.main()
