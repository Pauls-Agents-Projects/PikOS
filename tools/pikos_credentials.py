#!/usr/bin/env python3
"""Create copyable PikOS setup JSON for Netatmo or Garmin Connect.

The helper is intentionally interactive for secrets: passwords, MFA codes and
client secrets cannot be supplied on the command line.  It writes no token
store and never prints a login password, MFA code, authorization code or
short-lived access token.  Successful stdout is JSON only so it can be copied
without editing; progress and safety notes go to stderr.
"""

from __future__ import annotations

import argparse
from getpass import getpass
import importlib
import importlib.metadata
import json
import logging
import re
import secrets
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from typing import Any, Callable, Mapping, Sequence
import urllib.parse
import urllib.request
import webbrowser

try:  # Works both as ``python -m tools...`` and ``python tools/...py``.
    from tools import garmin_provision
except ImportError:  # pragma: no cover - exercised by the documented CLI form
    import garmin_provision  # type: ignore[no-redef]


MIN_GARMINCONNECT = (0, 3, 5)
NETATMO_TOKEN_URL = "https://api.netatmo.com/oauth2/token"
NETATMO_AUTHORIZE_URL = "https://api.netatmo.com/oauth2/authorize"
MAX_TOKEN_RESPONSE_BYTES = 16 * 1024


class CredentialHelperError(RuntimeError):
    """A deliberately redacted, user-facing helper failure."""


def _clean_text(name: str, value: Any, maximum_bytes: int,
                *, allow_empty: bool = False) -> str:
    if not isinstance(value, str):
        raise CredentialHelperError(f"The provider returned an invalid {name}")
    value = value.strip()
    if not value and not allow_empty:
        raise CredentialHelperError(f"The provider did not return a {name}")
    if len(value.encode("utf-8")) > maximum_bytes:
        raise CredentialHelperError(f"The provider returned an oversized {name}")
    if any(ord(character) < 0x20 for character in value):
        raise CredentialHelperError(f"The provider returned an invalid {name}")
    return value


def _numeric_version(value: str) -> tuple[int, int, int]:
    numbers = [int(part) for part in re.findall(r"\d+", value)[:3]]
    numbers.extend([0] * (3 - len(numbers)))
    return numbers[0], numbers[1], numbers[2]


def require_garmin_dependency(
    *,
    python_version: tuple[int, int, int] | None = None,
    version_lookup: Callable[[str], str] | None = None,
) -> str:
    """Validate the supported runtime without importing the provider client."""

    current_python = python_version or (
        sys.version_info.major,
        sys.version_info.minor,
        sys.version_info.micro,
    )
    if current_python < (3, 12, 0):
        raise CredentialHelperError("Garmin setup requires Python 3.12 or newer")
    lookup = version_lookup or importlib.metadata.version
    try:
        version = lookup("garminconnect")
    except importlib.metadata.PackageNotFoundError:
        raise CredentialHelperError(
            "Garmin setup requires garminconnect >= 0.3.5; install it with pip"
        ) from None
    except Exception:
        raise CredentialHelperError(
            "The installed garminconnect version could not be checked"
        ) from None
    if _numeric_version(version) < MIN_GARMINCONNECT:
        raise CredentialHelperError(
            "Garmin setup requires garminconnect 0.3.5 or newer"
        )
    return version


def load_garmin_class() -> type[Any]:
    require_garmin_dependency()
    try:
        module = importlib.import_module("garminconnect")
        garmin_class = getattr(module, "Garmin")
    except Exception:
        raise CredentialHelperError("garminconnect could not be imported") from None
    return garmin_class


def _garmin_display_name(client: Any) -> str:
    """Read already-populated profile state without making another request."""

    owners = (client, getattr(client, "client", None))
    for owner in owners:
        if owner is None:
            continue
        for attribute in ("display_name", "displayName"):
            value = getattr(owner, attribute, "")
            if isinstance(value, str) and value.strip():
                return _clean_text("Garmin display name", value, 128)
    return ""


def make_garmin_settings(domain: str, client_id: str, refresh_token: str,
                         display_name: str = "") -> dict[str, str]:
    """Validate Garmin's persistent DI fields and map them to PikOS keys."""

    if domain not in {"garmin.com", "garmin.cn"}:
        raise CredentialHelperError("Unsupported Garmin domain")
    client_id = _clean_text("Garmin client ID", client_id, 128)
    refresh_token = _clean_text("Garmin refresh token", refresh_token, 3072)
    display_name = _clean_text(
        "Garmin display name", display_name, 128, allow_empty=True
    )

    # Keep the helper and the firmware-side USB experiment on exactly the same
    # credential bounds while the latter remains available for protocol tests.
    probe = garmin_provision.canonical_payload(
        {
            "kind": "pikos.garmin.di",
            "schema": 1,
            "domain": domain,
            "client_id": client_id,
            "refresh_token": refresh_token,
        }
    )
    probe[:] = b"\x00" * len(probe)
    return {
        "garmin_domain": domain,
        "garmin_client_id": client_id,
        "garmin_refresh_token": refresh_token,
        "garmin_display_name": display_name,
    }


def authenticate_garmin(
    garmin_class: type[Any],
    domain: str,
    *,
    input_fn: Callable[[str], str] = input,
    secret_prompt: Callable[[str], str] = getpass,
) -> dict[str, str]:
    """Run one interactive Garmin login without creating a token-store file."""

    email = input_fn("Garmin account email: ").strip()
    password = secret_prompt("Garmin password: ")
    if not email or not password:
        password = ""
        raise CredentialHelperError("Garmin email and password are required")

    def prompt_mfa() -> str:
        code = secret_prompt("Garmin MFA code: ").strip()
        if not code:
            raise CredentialHelperError("A Garmin MFA code is required")
        return code

    previous_logging_disable = logging.root.manager.disable
    logging.disable(logging.CRITICAL)
    client: Any = None
    try:
        client = garmin_class(
            email,
            password,
            is_cn=domain == "garmin.cn",
            prompt_mfa=prompt_mfa,
            retry_attempts=1,
            verify_login=True,
        )
        # No tokenstore argument: python-garminconnect keeps this login in RAM.
        client.login()
        auth = getattr(client, "client", None)
        if auth is None:
            raise CredentialHelperError("Garmin did not return DI credentials")
        return make_garmin_settings(
            domain,
            getattr(auth, "di_client_id", "") or "",
            getattr(auth, "di_refresh_token", "") or "",
            _garmin_display_name(client),
        )
    except CredentialHelperError:
        raise
    except Exception:
        # Provider exceptions can contain URLs, account identifiers or request
        # state.  Deliberately replace them with a fixed diagnostic.
        raise CredentialHelperError(
            "Garmin authentication failed; wait before retrying and check the login"
        ) from None
    finally:
        password = ""
        email = ""
        garmin_provision.clear_client(client)
        logging.disable(previous_logging_disable)


def make_netatmo_settings(client_id: str, client_secret: str,
                          token_response: Mapping[str, Any]) -> dict[str, str]:
    """Return only refreshable Netatmo fields; omit the access token."""

    client_id = _clean_text("Netatmo client ID", client_id, 256)
    client_secret = _clean_text("Netatmo client secret", client_secret, 512)
    # A successful authorization-code response should contain both values.  We
    # validate the access token but intentionally never copy it into stdout.
    _clean_text("Netatmo access token", token_response.get("access_token"), 4096)
    refresh_token = _clean_text(
        "Netatmo refresh token", token_response.get("refresh_token"), 4096
    )
    return {
        "netatmo_client_id": client_id,
        "netatmo_client_secret": client_secret,
        "netatmo_refresh_token": refresh_token,
    }


def authorize_netatmo(client_id: str, port: int, timeout_seconds: float,
                      *, open_browser: bool) -> tuple[str, str]:
    """Open Netatmo authorization and return ``(code, redirect_uri)``."""

    client_id = _clean_text("Netatmo client ID", client_id, 256)
    state = secrets.token_urlsafe(24)
    redirect_uri = f"http://localhost:{port}/callback"
    result: dict[str, str] = {}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            parsed = urllib.parse.urlparse(self.path)
            if parsed.path != "/callback":
                self.send_error(404)
                return
            query = urllib.parse.parse_qs(parsed.query)
            if not secrets.compare_digest(query.get("state", [""])[0], state):
                self.send_error(400, "OAuth state mismatch")
                return
            if query.get("error", [""])[0]:
                result["error"] = "1"
                body = b"Authorization was not completed. You can close this tab."
            else:
                result["code"] = query.get("code", [""])[0]
                body = b"Authorization received. You can close this tab."
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_args: Any) -> None:
            pass

    try:
        server = HTTPServer(("127.0.0.1", port), Handler)
    except Exception:
        raise CredentialHelperError(
            "The local Netatmo callback port could not be opened"
        ) from None
    server.timeout = 0.25
    url = NETATMO_AUTHORIZE_URL + "?" + urllib.parse.urlencode(
        {
            "client_id": client_id,
            "redirect_uri": redirect_uri,
            "scope": "read_station",
            "state": state,
        }
    )
    print("Open this Netatmo authorization URL:", file=sys.stderr)
    print(url, file=sys.stderr)
    if open_browser:
        try:
            webbrowser.open(url)
        except Exception:
            print("The browser did not open; use the URL above.", file=sys.stderr)

    deadline = time.monotonic() + timeout_seconds
    try:
        while time.monotonic() < deadline and not result:
            server.handle_request()
    finally:
        server.server_close()
    if result.get("error"):
        raise CredentialHelperError("Netatmo authorization was declined or failed")
    code = result.get("code", "")
    if not code:
        raise CredentialHelperError("Netatmo authorization timed out")
    return code, redirect_uri


def exchange_netatmo_code(client_id: str, client_secret: str, code: str,
                          redirect_uri: str) -> dict[str, Any]:
    """Exchange the code with a bounded response and redacted failures."""

    body = urllib.parse.urlencode(
        {
            "grant_type": "authorization_code",
            "client_id": client_id,
            "client_secret": client_secret,
            "code": code,
            "redirect_uri": redirect_uri,
            "scope": "read_station",
        }
    ).encode("utf-8")
    request = urllib.request.Request(
        NETATMO_TOKEN_URL,
        data=body,
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            payload = response.read(MAX_TOKEN_RESPONSE_BYTES + 1)
    except Exception:
        raise CredentialHelperError("Netatmo token exchange failed") from None
    if len(payload) > MAX_TOKEN_RESPONSE_BYTES:
        raise CredentialHelperError("Netatmo returned an oversized token response")
    try:
        parsed = json.loads(payload)
    except Exception:
        raise CredentialHelperError("Netatmo returned an invalid token response") from None
    if not isinstance(parsed, dict):
        raise CredentialHelperError("Netatmo returned an invalid token response")
    return parsed


def run_netatmo(
    args: argparse.Namespace,
    *,
    input_fn: Callable[[str], str] = input,
    secret_prompt: Callable[[str], str] = getpass,
) -> dict[str, str]:
    client_id = args.client_id or input_fn("Netatmo client ID: ").strip()
    client_secret = secret_prompt("Netatmo client secret: ")
    if not client_id or not client_secret:
        client_secret = ""
        raise CredentialHelperError("Netatmo client ID and secret are required")
    tokens: dict[str, Any] = {}
    code = ""
    try:
        code, redirect_uri = authorize_netatmo(
            client_id, args.port, args.timeout, open_browser=not args.no_browser
        )
        tokens = exchange_netatmo_code(
            client_id, client_secret, code, redirect_uri
        )
        return make_netatmo_settings(client_id, client_secret, tokens)
    finally:
        tokens.clear()
        code = ""
        client_secret = ""


def run_garmin(
    args: argparse.Namespace,
    *,
    input_fn: Callable[[str], str] = input,
    secret_prompt: Callable[[str], str] = getpass,
) -> dict[str, str]:
    garmin_class = load_garmin_class()
    return authenticate_garmin(
        garmin_class,
        "garmin.cn" if args.china else "garmin.com",
        input_fn=input_fn,
        secret_prompt=secret_prompt,
    )


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="service", required=True)

    netatmo = subparsers.add_parser(
        "netatmo", help="authorize a Netatmo weather station"
    )
    netatmo.add_argument("--client-id", help="Netatmo application client ID")
    netatmo.add_argument("--port", type=int, default=8765)
    netatmo.add_argument("--timeout", type=float, default=300.0)
    netatmo.add_argument("--no-browser", action="store_true")

    garmin = subparsers.add_parser(
        "garmin", help="extract persistent Garmin Connect DI credentials"
    )
    garmin.add_argument("--china", action="store_true", help="use garmin.cn")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.service == "netatmo":
            if not 1024 <= args.port <= 65535 or not 5 <= args.timeout <= 900:
                raise CredentialHelperError("Invalid Netatmo callback settings")
            settings = run_netatmo(args)
        else:
            settings = run_garmin(args)
        print(json.dumps(settings, ensure_ascii=False, indent=2))
        print(
            "Paste these fields into the matching PikOS app, then clear this "
            "terminal: the JSON contains persistent credentials.",
            file=sys.stderr,
        )
        return 0
    except CredentialHelperError as exc:
        print(f"PikOS credential setup stopped: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("PikOS credential setup cancelled; nothing was saved.", file=sys.stderr)
        return 130
    except Exception:
        # Never echo an unexpected provider exception; it may carry request or
        # account data even when its type normally appears harmless.
        print("PikOS credential setup failed; nothing was saved.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
