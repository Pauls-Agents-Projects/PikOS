"""Small same-origin session client for PikOS development tools."""

from __future__ import annotations

import http.cookiejar
import threading
import urllib.parse
import urllib.request


class _OriginSession:
    def __init__(self, origin: str) -> None:
        self.origin = origin
        self.jar = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.HTTPCookieProcessor(self.jar)
        )
        self.ready = False
        self.lock = threading.Lock()

    def ensure(self, timeout: float) -> None:
        if self.ready:
            return
        with self.lock:
            if self.ready:
                return
            # Loading the portal is the same explicit local-presence action as
            # opening it in a browser. It establishes an HttpOnly, same-origin
            # session without exposing PikOS's long-lived remote API key.
            with self.opener.open(self.origin + "/", timeout=timeout) as response:
                while response.read(8192):
                    pass
            self.ready = True

    def open(self, request, timeout: float):
        method = request.get_method() if isinstance(
            request, urllib.request.Request
        ) else "GET"
        if method != "GET":
            self.ensure(timeout)
            request.add_unredirected_header("X-PikOS-Request", "1")
        return self.opener.open(request, timeout=timeout)


_sessions: dict[str, _OriginSession] = {}
_sessions_lock = threading.Lock()


def open_pikos(request, *, timeout: float):
    """Open a PikOS URL, establishing its per-boot session for mutations."""
    url = request.full_url if isinstance(request, urllib.request.Request) else request
    parsed = urllib.parse.urlsplit(url)
    origin = f"{parsed.scheme}://{parsed.netloc}"
    with _sessions_lock:
        session = _sessions.setdefault(origin, _OriginSession(origin))
    return session.open(request, timeout)
