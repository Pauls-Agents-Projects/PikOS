import argparse
import contextlib
import importlib.metadata
import io
import json
import unittest
from unittest import mock

from tools import netatmo_auth, pikos_credentials


class FakeGarmin:
    latest = None

    def __init__(self, username, password, **options):
        self.username = username
        self.password = password
        self.options = options
        self.display_name = "FixtureRunner"
        self.client = type(
            "FakeAuth",
            (),
            {
                "di_client_id": "FIXTURE_CLIENT_ID",
                "di_refresh_token": "FIXTURE_REFRESH_TOKEN",
                "di_token": "SHORT_LIVED_ACCESS_TOKEN",
            },
        )()
        FakeGarmin.latest = self

    def login(self):
        self.options["prompt_mfa"]()


class PikOSCredentialHelperTests(unittest.TestCase):
    def test_garmin_login_outputs_only_persistent_device_fields(self):
        prompts = iter(["fixture-password", "123456"])
        settings = pikos_credentials.authenticate_garmin(
            FakeGarmin,
            "garmin.com",
            input_fn=lambda _prompt: "fixture@example.test",
            secret_prompt=lambda _prompt: next(prompts),
        )

        self.assertEqual(
            settings,
            {
                "garmin_domain": "garmin.com",
                "garmin_client_id": "FIXTURE_CLIENT_ID",
                "garmin_refresh_token": "FIXTURE_REFRESH_TOKEN",
                "garmin_display_name": "FixtureRunner",
            },
        )
        serialized = json.dumps(settings)
        for forbidden in (
            "fixture-password",
            "123456",
            "fixture@example.test",
            "SHORT_LIVED_ACCESS_TOKEN",
            "di_token",
        ):
            self.assertNotIn(forbidden, serialized)
        self.assertIsNone(FakeGarmin.latest.client.di_token)
        self.assertIsNone(FakeGarmin.latest.client.di_refresh_token)
        self.assertIsNone(FakeGarmin.latest.client.di_client_id)
        self.assertIsNone(FakeGarmin.latest.password)

    def test_netatmo_output_omits_authorization_code_and_access_token(self):
        args = argparse.Namespace(
            client_id="FIXTURE_NETATMO_ID",
            port=8765,
            timeout=30.0,
            no_browser=True,
        )
        with mock.patch.object(
            pikos_credentials,
            "authorize_netatmo",
            return_value=("ONE_TIME_AUTHORIZATION_CODE", "http://localhost/callback"),
        ), mock.patch.object(
            pikos_credentials,
            "exchange_netatmo_code",
            return_value={
                "access_token": "SHORT_LIVED_NETATMO_ACCESS",
                "refresh_token": "FIXTURE_NETATMO_REFRESH",
            },
        ):
            settings = pikos_credentials.run_netatmo(
                args, secret_prompt=lambda _prompt: "FIXTURE_NETATMO_SECRET"
            )

        self.assertEqual(
            settings,
            {
                "netatmo_client_id": "FIXTURE_NETATMO_ID",
                "netatmo_client_secret": "FIXTURE_NETATMO_SECRET",
                "netatmo_refresh_token": "FIXTURE_NETATMO_REFRESH",
            },
        )
        serialized = json.dumps(settings)
        self.assertNotIn("access_token", serialized)
        self.assertNotIn("SHORT_LIVED_NETATMO_ACCESS", serialized)
        self.assertNotIn("ONE_TIME_AUTHORIZATION_CODE", serialized)

    def test_garmin_dependency_version_guards_are_offline(self):
        with self.assertRaisesRegex(
            pikos_credentials.CredentialHelperError, "Python 3.12"
        ):
            pikos_credentials.require_garmin_dependency(
                python_version=(3, 11, 9), version_lookup=lambda _name: "0.9.0"
            )
        with self.assertRaisesRegex(
            pikos_credentials.CredentialHelperError, "0.3.5 or newer"
        ):
            pikos_credentials.require_garmin_dependency(
                python_version=(3, 12, 0), version_lookup=lambda _name: "0.3.4"
            )

        def missing(_name):
            raise importlib.metadata.PackageNotFoundError

        with self.assertRaisesRegex(
            pikos_credentials.CredentialHelperError, "install it with pip"
        ):
            pikos_credentials.require_garmin_dependency(
                python_version=(3, 12, 0), version_lookup=missing
            )
        self.assertEqual(
            pikos_credentials.require_garmin_dependency(
                python_version=(3, 12, 0), version_lookup=lambda _name: "0.3.5"
            ),
            "0.3.5",
        )

    def test_success_stdout_is_json_only(self):
        stdout = io.StringIO()
        stderr = io.StringIO()
        fixture = {
            "garmin_domain": "garmin.com",
            "garmin_client_id": "CLIENT",
            "garmin_refresh_token": "REFRESH",
            "garmin_display_name": "Runner",
        }
        with mock.patch.object(
            pikos_credentials, "run_garmin", return_value=fixture
        ), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            result = pikos_credentials.main(["garmin"])

        self.assertEqual(result, 0)
        self.assertEqual(json.loads(stdout.getvalue()), fixture)
        self.assertNotIn("Garmin account", stdout.getvalue())
        self.assertIn("persistent credentials", stderr.getvalue())

    def test_unexpected_errors_are_redacted(self):
        stderr = io.StringIO()
        with mock.patch.object(
            pikos_credentials,
            "run_garmin",
            side_effect=RuntimeError(
                "password=LEAK access_token=LEAK email=fixture@example.test"
            ),
        ), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
            result = pikos_credentials.main(["garmin"])

        self.assertEqual(result, 2)
        self.assertEqual(
            stderr.getvalue(),
            "PikOS credential setup failed; nothing was saved.\n",
        )
        self.assertNotIn("LEAK", stderr.getvalue())
        self.assertNotIn("fixture@example.test", stderr.getvalue())

    def test_legacy_netatmo_launcher_rejects_command_line_secret_safely(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            result = netatmo_auth.main(
                ["--client-id", "FIXTURE", "--client-secret", "DO_NOT_ECHO"]
            )
        self.assertEqual(result, 2)
        self.assertNotIn("DO_NOT_ECHO", stderr.getvalue())
        self.assertIn("hidden prompt", stderr.getvalue())

    def test_control_characters_and_unknown_domains_are_rejected(self):
        with self.assertRaises(pikos_credentials.CredentialHelperError):
            pikos_credentials.make_garmin_settings(
                "garmin.test", "CLIENT", "REFRESH", "Runner"
            )
        with self.assertRaises(pikos_credentials.CredentialHelperError):
            pikos_credentials.make_netatmo_settings(
                "CLIENT", "bad\nsecret", {
                    "access_token": "ACCESS",
                    "refresh_token": "REFRESH",
                }
            )


if __name__ == "__main__":
    unittest.main()
