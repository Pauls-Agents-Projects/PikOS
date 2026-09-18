#!/usr/bin/env python3
"""Legacy PikOS Garmin DI-token USB provisioner (release-disabled skeleton).

Use ``pikos_credentials.py garmin`` for normal interactive setup. This module
remains for its shared credential bounds and mock-only serial-envelope tests;
the real USB path intentionally refuses to run. ``--self-test`` uses fake
values only.
"""

from __future__ import annotations

import argparse
import base64
import binascii
from getpass import getpass
import hashlib
import importlib.metadata
import json
import logging
import re
import secrets
import sys
import time
from typing import Any
import zlib


MIN_GARMINCONNECT = (0, 3, 5)
MAX_PAYLOAD_BYTES = 4096
MAX_WIRE_BYTES = 8192
PROVISIONING_ENABLED = False

HELLO_PREFIX = b"PIKOS-PROVISION-HELLO/1 "
READY_PREFIX = b"PIKOS-PROVISION-READY/1 "
DATA_PREFIX = b"PIKOS-PROVISION/1 "
OK_PREFIX = b"PIKOS-PROVISION-OK/1 "


class ProvisioningError(RuntimeError):
    """A deliberately non-secret user-facing provisioning failure."""


def _numeric_version(value: str) -> tuple[int, int, int]:
    numbers = [int(part) for part in re.findall(r"\d+", value)[:3]]
    numbers.extend([0] * (3 - len(numbers)))
    return numbers[0], numbers[1], numbers[2]


def require_dependencies(require_serial: bool = True) -> None:
    if sys.version_info < (3, 12):
        raise ProvisioningError("Python 3.12 or newer is required")
    try:
        version = importlib.metadata.version("garminconnect")
    except importlib.metadata.PackageNotFoundError as exc:
        raise ProvisioningError(
            "garminconnect >= 0.3.5 is required; install the documented helper dependencies"
        ) from exc
    if _numeric_version(version) < MIN_GARMINCONNECT:
        raise ProvisioningError("garminconnect 0.3.5 or newer is required")
    if require_serial:
        try:
            importlib.metadata.version("pyserial")
        except importlib.metadata.PackageNotFoundError as exc:
            raise ProvisioningError("pyserial is required for USB provisioning") from exc


def canonical_payload(fields: dict[str, Any]) -> bytearray:
    expected_fields = {"kind", "schema", "domain", "client_id", "refresh_token"}
    if set(fields) != expected_fields:
        raise ProvisioningError("Internal provisioning field-set error")
    if fields.get("kind") != "pikos.garmin.di" or fields.get("schema") != 1:
        raise ProvisioningError("Internal provisioning schema error")
    if fields.get("domain") not in {"garmin.com", "garmin.cn"}:
        raise ProvisioningError("Unsupported Garmin domain")
    bounds = {"client_id": 128, "refresh_token": 3072}
    for name, limit in bounds.items():
        value = fields.get(name)
        if not isinstance(value, str) or not value or len(value.encode("utf-8")) > limit:
            raise ProvisioningError(f"Garmin returned an invalid {name.replace('_', ' ')}")
        if any(ord(character) < 0x20 for character in value):
            raise ProvisioningError(f"Garmin returned an invalid {name.replace('_', ' ')}")
    encoded = bytearray(
        json.dumps(fields, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
    )
    if len(encoded) > MAX_PAYLOAD_BYTES:
        encoded[:] = b"\x00" * len(encoded)
        raise ProvisioningError("Garmin token bundle exceeds the safe device limit")
    return encoded


def make_wire_envelope(fields: dict[str, Any], nonce: str) -> tuple[bytearray, str]:
    if not re.fullmatch(r"[0-9a-f]{32}", nonce):
        raise ProvisioningError("Invalid provisioning nonce")
    payload = canonical_payload(fields)
    try:
        digest = hashlib.sha256(payload).hexdigest()
        envelope = {
            "crc32": f"{zlib.crc32(payload) & 0xFFFFFFFF:08x}",
            "nonce": nonce,
            "payload_b64": base64.b64encode(payload).decode("ascii"),
            "payload_len": len(payload),
            "sha256": digest,
        }
        wire = bytearray(DATA_PREFIX)
        wire.extend(json.dumps(envelope, sort_keys=True, separators=(",", ":")).encode())
        wire.extend(b"\n")
        if len(wire) > MAX_WIRE_BYTES:
            wire[:] = b"\x00" * len(wire)
            raise ProvisioningError("Provisioning envelope exceeds the safe serial limit")
        return wire, digest
    finally:
        payload[:] = b"\x00" * len(payload)


def _read_line(port: Any, deadline: float, limit: int = 512) -> bytes:
    line = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(1)
        if not chunk:
            continue
        if chunk == b"\n":
            return bytes(line).rstrip(b"\r")
        line.extend(chunk)
        if len(line) > limit:
            line[:] = b"\x00" * len(line)
            raise ProvisioningError("Device returned an oversized serial response")
    line[:] = b"\x00" * len(line)
    raise ProvisioningError("Timed out waiting for the PikOS USB receiver")


def wait_for_line(port: Any, expected: bytes, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        if _read_line(port, deadline) == expected:
            return
    raise ProvisioningError("PikOS did not confirm the provisioning request")


def open_receiver(port_name: str, baud: int, timeout_seconds: float) -> tuple[Any, str]:
    try:
        import serial
    except ImportError as exc:
        raise ProvisioningError("pyserial is required for USB provisioning") from exc

    nonce = secrets.token_hex(16)
    try:
        port = serial.Serial(
            port_name,
            baudrate=baud,
            timeout=0.25,
            write_timeout=timeout_seconds,
            exclusive=True,
        )
    except Exception as exc:
        raise ProvisioningError("Could not open the selected serial port") from exc

    hello = HELLO_PREFIX + json.dumps(
        {"kind": "garmin-di", "nonce": nonce}, sort_keys=True, separators=(",", ":")
    ).encode() + b"\n"
    try:
        port.reset_input_buffer()
        port.write(hello)
        port.flush()
        wait_for_line(port, READY_PREFIX + nonce.encode("ascii"), timeout_seconds)
        return port, nonce
    except Exception:
        port.close()
        raise


def authenticate(domain: str) -> tuple[dict[str, Any], Any]:
    # Do not let dependency diagnostics or HTTP exceptions print request state.
    previous_logging_disable = logging.root.manager.disable
    logging.disable(logging.CRITICAL)
    try:
        from garminconnect import Garmin
    except ImportError as exc:
        logging.disable(previous_logging_disable)
        raise ProvisioningError("garminconnect could not be imported") from exc

    email = input("Garmin account email: ").strip()
    password = getpass("Garmin password: ")
    if not email or not password:
        raise ProvisioningError("Garmin email and password are required")

    client = Garmin(
        email,
        password,
        is_cn=domain == "garmin.cn",
        prompt_mfa=lambda: getpass("Garmin MFA code: ").strip(),
        retry_attempts=1,
        verify_login=True,
    )
    password = ""
    try:
        # No tokenstore argument: the helper intentionally writes no token file.
        client.login()
        auth = client.client
        fields = {
            "kind": "pikos.garmin.di",
            "schema": 1,
            "domain": domain,
            "client_id": auth.di_client_id or "",
            "refresh_token": auth.di_refresh_token or "",
        }
        probe = canonical_payload(fields)
        probe[:] = b"\x00" * len(probe)
        return fields, client
    except Exception as exc:
        client.password = None
        raise ProvisioningError(
            "Garmin authentication failed; no token was sent (wait before retrying)"
        ) from exc
    finally:
        logging.disable(previous_logging_disable)


def clear_client(client: Any) -> None:
    try:
        client.password = None
        client.username = None
        client.client.di_token = None
        client.client.di_refresh_token = None
        client.client.di_client_id = None
    except Exception:
        pass


def provision(args: argparse.Namespace) -> None:
    receiver, nonce = open_receiver(args.port, args.baud, args.timeout)
    fields: dict[str, Any] = {}
    client: Any = None
    wire = bytearray()
    try:
        fields, client = authenticate("garmin.cn" if args.china else "garmin.com")
        wire, digest = make_wire_envelope(fields, nonce)
        receiver.write(wire)
        receiver.flush()
        expected = OK_PREFIX + nonce.encode("ascii") + b" " + digest.encode("ascii")
        wait_for_line(receiver, expected, args.timeout)
        print("Garmin connection stored by PikOS. No password or token was saved by this helper.")
    finally:
        if wire:
            wire[:] = b"\x00" * len(wire)
        fields.clear()
        clear_client(client)
        receiver.close()


def self_test() -> None:
    nonce = "00" * 16
    fake = {
        "kind": "pikos.garmin.di",
        "schema": 1,
        "domain": "garmin.com",
        "client_id": "TEST_CLIENT_ID",
        "refresh_token": "NOT_A_REAL_TOKEN",
    }
    wire, digest = make_wire_envelope(fake, nonce)
    try:
        assert wire.startswith(DATA_PREFIX) and wire.endswith(b"\n")
        envelope = json.loads(wire[len(DATA_PREFIX) :])
        payload = base64.b64decode(envelope["payload_b64"], validate=True)
        assert len(payload) == envelope["payload_len"]
        assert hashlib.sha256(payload).hexdigest() == digest == envelope["sha256"]
        assert f"{binascii.crc32(payload) & 0xFFFFFFFF:08x}" == envelope["crc32"]
        decoded = json.loads(payload)
        assert "di_token" not in decoded and "password" not in decoded and "email" not in decoded
        assert "display_name" not in decoded
        assert decoded["refresh_token"] == "NOT_A_REAL_TOKEN"
    finally:
        wire[:] = b"\x00" * len(wire)
    print("Garmin provisioning envelope self-test passed (fake values only).")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="PikOS USB serial device, for example /dev/cu.usbserial-…")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--china", action="store_true", help="Use garmin.cn instead of garmin.com")
    parser.add_argument("--self-test", action="store_true", help="Test only the fake serial envelope")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.self_test:
        self_test()
        return 0
    try:
        if not PROVISIONING_ENABLED:
            raise ProvisioningError(
                "Provisioning is release-disabled until firmware refresh/receiver hardware tests pass; "
                "see docs/garmin-connect-activity.md"
            )
        require_dependencies()
        if not args.port:
            raise ProvisioningError("--port is required")
        if args.baud < 9600 or args.baud > 921600 or args.timeout <= 0:
            raise ProvisioningError("Invalid serial settings")
        provision(args)
        return 0
    except ProvisioningError as exc:
        print(f"Garmin provisioning stopped: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("Garmin provisioning cancelled; no token was sent.", file=sys.stderr)
        return 130
    except Exception:
        # Raw dependency/network exceptions may contain request state. Never echo them.
        print("Garmin provisioning failed; no token was sent.", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
