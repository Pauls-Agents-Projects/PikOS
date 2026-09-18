"""Release regressions for generated and LaMetric icon selection paths."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class IconDefaultsAndActivityMetricsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.config_h = (ROOT / "include" / "Config.h").read_text()
        cls.config_cpp = (ROOT / "src" / "Config.cpp").read_text()
        cls.engine = (ROOT / "src" / "AppEngine.cpp").read_text()
        cls.display = (ROOT / "src" / "PixelDisplay.cpp").read_text()
        cls.icons = (ROOT / "src" / "LaMetricIcons.cpp").read_text()
        cls.web = (ROOT / "include" / "WebUi.h").read_text()

    def test_requested_gallery_defaults_are_persisted_and_queued(self) -> None:
        for declaration in (
            "uint32_t climateWindIconId = 2298;",
            "uint32_t climateRainIconId = 72;",
            "uint32_t activityIconId = 397;",
            "uint32_t counterIconId = 35196;",
        ):
            self.assertIn(declaration, self.config_h)
        for queued in (
            "addId(config.climateWindIconId);",
            "addId(config.climateRainIconId);",
            "addId(11999);",
        ):
            self.assertIn(queued, self.icons)

    def test_thermometer_lowest_band_is_a_plus_inside_white_outline(self) -> None:
        thermometer = self.display.split(
            "case TinyIcon::Thermometer:", 1
        )[1].split("case TinyIcon::Weather:", 1)[0]
        self.assertIn("horizontal(1, 3, 5, primary);", thermometer)
        self.assertIn("pixel(2, 6, primary);", thermometer)
        self.assertIn("for (uint8_t row = 1; row < fillRows; ++row)", thermometer)
        self.assertIn("horizontal(0, 4, 5, accent);", thermometer)
        self.assertIn("horizontal(1, 3, 6, accent);", thermometer)
        self.assertIn("pixel(2, 7, accent);", thermometer)

    def test_metric_specific_and_safe_fallback_icons_are_used(self) -> None:
        self.assertIn("return config.climateWindIconId;", self.engine)
        self.assertIn("return config.climateRainIconId;", self.engine)
        self.assertIn(
            "currentTravelIconId_ != 0 ? currentTravelIconId_ : 11999",
            self.engine,
        )
        self.assertIn("TinyIcon::Hourglass", self.engine)
        self.assertNotIn(
            "display_.drawIcon(TinyIcon::Clock, 0, 0, color, nowMs",
            self.engine,
        )

    def test_activity_metric_choices_round_trip_and_drive_the_renderer(self) -> None:
        keys = (
            "activity_show_steps",
            "activity_show_daily_distance",
            "activity_show_latest_distance",
            "activity_show_latest_duration",
            "activity_show_latest_calories",
        )
        for key in keys:
            self.assertIn(f'root["{key}"]', self.config_cpp)
            self.assertIn(f'id="{key}"', self.web)
        for member in (
            "config.activityShowSteps",
            "config.activityShowDailyDistance",
            "config.activityShowLatestDistance",
            "config.activityShowLatestDuration",
            "config.activityShowLatestCalories",
        ):
            self.assertIn(member, self.engine)
        self.assertIn("activityMetricCursor_++ % 5U", self.engine)

    def test_web_defaults_match_firmware_defaults(self) -> None:
        defaults = self.web.split("const defaults={", 1)[1].split("};", 1)[0]
        for pair in (
            "climate_wind_icon_id:2298",
            "climate_rain_icon_id:72",
            "activity_icon_id:397",
            "counter_icon_id:35196",
        ):
            self.assertIn(pair, defaults)
        self.assertIn("iconId:11999", self.web)

    def test_gallery_response_uses_bounded_tls_scratch_not_heap_string(self) -> None:
        download = self.icons.split(
            "bool LaMetricIcons::download(uint32_t iconId)", 1
        )[1].split("uint32_t LaMetricIcons::faviconId", 1)[0]
        self.assertIn("kMaximumGalleryPayloadBytes", download)
        self.assertIn("payloadScratch.allocate", download)
        self.assertIn("stream->readBytes", download)
        self.assertNotIn("http.getString()", download)


if __name__ == "__main__":
    unittest.main()
