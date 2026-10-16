"""Release-candidate contracts for the portal, crash journal, and UI."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ReleaseHardeningTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.web = (ROOT / "include" / "WebUi.h").read_text(encoding="utf-8")
        cls.portal = (ROOT / "src" / "WebPortal.cpp").read_text(
            encoding="utf-8"
        )
        cls.config = (ROOT / "src" / "Config.cpp").read_text(
            encoding="utf-8"
        )
        cls.crash = (ROOT / "src" / "CrashBreadcrumb.cpp").read_text(
            encoding="utf-8"
        )
        cls.network = (ROOT / "src" / "NetworkManager.cpp").read_text(
            encoding="utf-8"
        )
        cls.preview = (ROOT / "tools" / "preview_web_ui.py").read_text(
            encoding="utf-8"
        )

    def test_notification_centre_owns_cached_content(self) -> None:
        live = self.web.index('data-ui="liveDisplay"')
        centre = self.web.index('data-ui="notificationCentre"')
        apps = self.web.index('data-section="apps"')
        self.assertLess(live, centre)
        self.assertLess(centre, apps)
        self.assertIn('id="notice-all"', self.web)
        self.assertIn('id="notification-feed"', self.web)
        for app in ("rss", "calendar", "reddit", "travel", "activity"):
            self.assertIn(f'id="notice-{app}"', self.web)
        self.assertIn("entries.sort((left,right)=>", self.web)
        self.assertIn("activeNotificationApp===app", self.web)
        self.assertIn('id="diag-paused"', self.web)
        self.assertNotIn('id="rss-stored-panel"', self.web)
        self.assertNotIn('class="stored-content', self.web)

    def test_notification_feed_has_provider_timestamps(self) -> None:
        header = (ROOT / "include" / "DataServices.h").read_text(
            encoding="utf-8"
        )
        providers = (ROOT / "src" / "DataServices.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("uint32_t publishedAt = 0;", header)
        self.assertIn("parseFeedTimestamp(timestamp_)", providers)
        for tag in ("pubdate", "published", "updated"):
            self.assertIn(f'lower == "{tag}"', providers)
        self.assertIn('item["timestamp"] = headline.publishedAt', self.portal)
        # The local visual test fixture mirrors the numeric epoch contract and
        # deliberately interleaves RSS and Reddit publication times.
        self.assertIn('"timestamp": 1792176300', self.preview)
        self.assertIn('"timestamp": 1792175100', self.preview)
        self.assertIn('"starts_at": 1792337400', self.preview)

    def test_apps_have_no_nested_vertical_scroller(self) -> None:
        self.assertIn(
            ".app-box{position:relative;flex:0 0 min(620px,calc(100vw - 72px));height:auto;min-height:0;overflow:visible",
            self.web,
        )
        self.assertIn("host.onwheel=event=>", self.web)
        self.assertIn("host.scrollLeft+=event.deltaY", self.web)

    def test_tagline_is_removed(self) -> None:
        self.assertNotIn("Sinu oluline info kaunilt 256 pikslil", self.web)
        self.assertNotIn("beautifully distilled to 256 pixels", self.web)
        self.assertNotIn('data-i18n="subtitle"', self.web)

    def test_browser_mutations_require_same_origin_session(self) -> None:
        self.assertIn("HttpOnly; SameSite=Strict", self.portal)
        self.assertIn('server_.header("X-PikOS-Request") != "1"', self.portal)
        self.assertIn("if (!validRequestHost()) return false;", self.portal)
        self.assertIn('"X-PikOS-Request":"1"', self.web)
        for handler in (
            "handleBrightnessPreview",
            "handleSaveConfig",
            "handleButton",
            "handleCounter",
        ):
            body = self.portal.split(f"void WebPortal::{handler}", 1)[1]
            body = body.split("\n}", 1)[0]
            self.assertIn("checkAdminSession()", body)

    def test_browser_config_never_contains_the_remote_api_key(self) -> None:
        self.assertIn(
            'root["api_key"] = includeSecrets ? config.apiKey : String("");',
            self.config,
        )

    def test_setup_network_has_a_per_device_password(self) -> None:
        self.assertIn("config_.apiKey.substring(keyLength - 12)", self.network)
        self.assertNotIn('WiFi.softAP(apName_.c_str(), "pixelos32"', self.network)

    def test_https_clients_verify_public_ca_bundle(self) -> None:
        sources = self.portal + (ROOT / "src" / "DataServices.cpp").read_text() + (
            ROOT / "src" / "LaMetricIcons.cpp"
        ).read_text()
        self.assertNotIn("setInsecure()", sources)
        self.assertIn("configureTrustedTlsClient", sources)
        self.assertIn(
            "board_build.embed_files = data/cert/x509_crt_bundle.bin",
            (ROOT / "platformio.ini").read_text(),
        )
        self.assertTrue((ROOT / "data" / "cert" / "x509_crt_bundle.bin").is_file())

    def test_crash_journal_is_bounded_and_wear_conscious(self) -> None:
        self.assertIn("constexpr uint8_t kHistoryCapacity = 4;", self.crash)
        self.assertIn("if (!snapshot.available || !snapshot.crashRelated) return;", self.crash)
        self.assertIn("recentCrashBreadcrumbs", self.crash)
        self.assertIn('createNestedArray("recent_crashes")', self.portal)

    def test_sync_faults_are_contained_and_leave_a_real_core_dump(self) -> None:
        providers = (ROOT / "src" / "DataServices.cpp").read_text(
            encoding="utf-8"
        )
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        partitions = (ROOT / "partitions.csv").read_text(encoding="utf-8")
        self.assertIn("quarantinedProviderMask_", providers)
        self.assertIn("kProviderFailureQuarantineThreshold = 3", providers)
        self.assertIn("previous.context == pixelos::CrashContext::Sync", main)
        self.assertIn("data, coredump", partitions)


if __name__ == "__main__":
    unittest.main()
