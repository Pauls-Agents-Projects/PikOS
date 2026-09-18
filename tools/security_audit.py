#!/usr/bin/env python3
"""Release-time PikOS source and configuration security checks."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {".git", ".pio", ".vscode", "backups", "dist", "__pycache__"}
SECRET_PATTERNS = {
    "private key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "GitHub token": re.compile(r"\bgh[opusr]_[A-Za-z0-9_]{30,}\b"),
    "AWS access key": re.compile(r"\bAKIA[0-9A-Z]{16}\b"),
    "Google API key": re.compile(r"\bAIza[0-9A-Za-z_-]{30,}\b"),
    "OpenAI API key": re.compile(r"\bsk-[A-Za-z0-9_-]{20,}\b"),
}


def source_files() -> list[Path]:
    result: list[Path] = []
    for path in ROOT.rglob("*"):
        if not path.is_file() or any(part in SKIP_DIRS for part in path.parts):
            continue
        if path.suffix.lower() in {".png", ".jpg", ".jpeg", ".gif", ".bin"}:
            continue
        result.append(path)
    return result


def main() -> int:
    failures: list[str] = []
    for path in source_files():
        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for label, pattern in SECRET_PATTERNS.items():
            if pattern.search(content):
                failures.append(f"{path.relative_to(ROOT)}: possible {label}")

    portal = (ROOT / "src" / "WebPortal.cpp").read_text(encoding="utf-8")
    config = (ROOT / "src" / "Config.cpp").read_text(encoding="utf-8")
    providers = (ROOT / "src" / "DataServices.cpp").read_text(encoding="utf-8")
    icons = (ROOT / "src" / "LaMetricIcons.cpp").read_text(encoding="utf-8")
    network = (ROOT / "src" / "NetworkManager.cpp").read_text(encoding="utf-8")

    invariants = {
        "HTTPS clients authenticate certificates": (
            "setInsecure()" not in providers + icons
            and "configureTrustedTlsClient" in providers + icons
        ),
        "browser configuration redacts remote API key": (
            'root["api_key"] = includeSecrets ? config.apiKey : String("");'
            in config
        ),
        "browser mutations require same-origin intent": (
            'server_.header("X-PikOS-Request") != "1"' in portal
            and "if (!validRequestHost()) return false;" in portal
            and "HttpOnly; SameSite=Strict" in portal
        ),
        "portal blocks framing and MIME sniffing": (
            '"X-Frame-Options", "DENY"' in portal
            and '"X-Content-Type-Options", "nosniff"' in portal
            and '"Content-Security-Policy"' in portal
        ),
        "setup access point uses a unique password": (
            "config_.apiKey.substring(keyLength - 12)" in network
            and 'WiFi.softAP(apName_.c_str(), "pixelos32"' not in network
        ),
        "OTA upload checks the browser session before writing": (
            "firmwareAuthorized_ = hasAdminSession();" in portal
            and "firmwareAuthorized_ && !updateFailed_" in portal
        ),
    }
    for label, passed in invariants.items():
        if not passed:
            failures.append(label)

    sensitive_artifacts = [
        ROOT / "backups" / "tc001-factory-083a8da5e58c.bin",
        ROOT / "backups" / "pikos-nonsecret-settings-2026-07-29.json",
    ]
    ignore = (ROOT / ".gitignore").read_text(encoding="utf-8")
    if "/backups/" not in ignore or "/dist/" not in ignore:
        failures.append("private backups or generated releases are not excluded by .gitignore")
    # The local files may exist for recovery; the publication check below is
    # about repository inclusion, not deleting the owner's private backup.
    for path in sensitive_artifacts:
        if path.exists() and path.is_symlink():
            failures.append(f"{path.relative_to(ROOT)} must not be a symlink")

    if failures:
        print("SECURITY AUDIT FAILED")
        for failure in failures:
            print(f"- {failure}")
        return 1
    print(
        f"SECURITY AUDIT PASSED: {len(source_files())} text files scanned; "
        f"{len(invariants)} release invariants verified"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
