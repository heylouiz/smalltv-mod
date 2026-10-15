"""HTTP client for the device's /api/companion/* routes (blocking; the
scheduler runs it in a worker thread)."""
from __future__ import annotations

import logging
from dataclasses import dataclass

import requests
from requests.auth import HTTPDigestAuth

from .frame import Rect, rgb565

log = logging.getLogger(__name__)


class NotActive(Exception):
    """The device is showing another mode; pushes are refused until it is back."""


@dataclass
class Status:
    epoch: int
    active: bool
    live: bool
    glyphs: bool
    clock: dict
    heap: int | None = None


class Device:
    def __init__(self, host: str, user: str = "", password: str = "", timeout: float = 15):
        self.base = host if host.startswith("http") else f"http://{host}"
        self.base = self.base.rstrip("/")
        self.session = requests.Session()
        if password:
            # Digest: the first request takes the 401 challenge, later ones
            # reuse its nonce, so a frame upload is not sent twice.
            self.session.auth = HTTPDigestAuth(user or "admin", password)
        self.timeout = timeout

    def _check(self, r: requests.Response) -> dict:
        if r.status_code == 409:
            raise NotActive()
        try:
            body = r.json()
        except ValueError:
            r.raise_for_status()
            raise
        if not r.ok:
            raise RuntimeError(f"{r.request.method} {r.url}: {r.status_code} {body.get('error')}")
        return body

    def status(self, heartbeat: bool = True) -> Status:
        r = self.session.get(f"{self.base}/api/companion/status",
                             params={"hb": "1"} if heartbeat else None, timeout=self.timeout)
        d = self._check(r)
        return Status(epoch=d["epoch"], active=d["active"], live=d["live"],
                      glyphs=d.get("glyphs", False), clock=d.get("clock", {}), heap=d.get("heap"))

    def send_rect(self, rect: Rect, pixels: bytes) -> int:
        """Pushes one rectangle; returns the device epoch after it."""
        assert len(pixels) == rect.w * rect.h * 2
        r = self.session.post(f"{self.base}/api/companion/rect",
                              params={"x": rect.x, "y": rect.y, "w": rect.w, "h": rect.h},
                              files={"f": ("rect.bin", pixels, "application/octet-stream")},
                              timeout=self.timeout)
        return self._check(r)["epoch"]

    def send_frame_rects(self, arr, rects: list[Rect]) -> int:
        epoch = 0
        for rc in rects:
            epoch = self.send_rect(rc, rgb565(arr[rc.y:rc.y2, rc.x:rc.x2]))
        return epoch

    def set_clock(self, enabled: bool, box: Rect | None, bg: int, fg: int) -> None:
        body = {"enabled": enabled, "bg": bg, "fg": fg}
        if box is not None:
            body.update(x=box.x, y=box.y, w=box.w, h=box.h)
        self._check(self.session.post(f"{self.base}/api/companion/clock", json=body, timeout=self.timeout))

    def upload_glyphs(self, atlas: bytes) -> None:
        r = self.session.post(f"{self.base}/api/companion/glyphs",
                              files={"f": ("glyphs.bin", atlas, "application/octet-stream")},
                              timeout=self.timeout)
        self._check(r)
