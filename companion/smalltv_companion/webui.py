"""A local settings page with a live preview, served while `run` runs.

The page edits config.toml (as JSON on the wire), renders unsaved drafts
with the running plugins' data, and on save writes the file and swaps the
new config into the running App without a restart. It listens on
127.0.0.1 only: the config holds the device password and the Steam key.
"""
from __future__ import annotations

import io
import json
import logging
import math
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from importlib import resources
from pathlib import Path

from . import config
from .app import App, _Active
from .plugins import Interrupt, available_plugins

log = logging.getLogger(__name__)


def _png(img) -> bytes:
    buf = io.BytesIO()
    img.save(buf, "PNG")
    return buf.getvalue()


def _sample_note(text: str = "Notification preview"):
    from .plugins.notifications import Note
    return Note("smalltv-companion", text, "This is what a notification looks like on the SmallTV.")


def render_draft(app: App, data: dict, screen: str | None = None, notify: bool = False):
    """The screen `data` would produce, drawn with the running plugins."""
    cfg = config.parse(data)
    plugins = {n: p for n, p in app.plugins.items() if n in cfg.used_plugins()}
    if screen:
        cfg.screens = [s for s in cfg.screens if s.name == screen]
    draft = App(cfg, plugins=plugins)
    note = plugins.get("notifications")
    if notify and note is not None:
        draft.active = _Active(note, Interrupt(0, key=_sample_note()), math.inf)
    return draft.preview()


def font_families() -> list[str]:
    """Installed font families, for the font fields' suggestions."""
    import shutil
    import subprocess
    if not shutil.which("fc-list"):
        return []
    out = subprocess.run(["fc-list", ":", "family"], capture_output=True, text=True).stdout
    return sorted({line.split(",")[0].strip() for line in out.splitlines() if line.strip()}, key=str.lower)


def state(app: App, path: Path) -> dict:
    screens = app.visible_screens()
    current = None
    if app.active:
        current = f"{app.active.plugin.name} (interruption)"
    elif screens:
        current = screens[app.screen_idx % len(screens)].name
    return {
        "path": str(path),
        "config": config.load_raw(path),
        "defaults": config.defaults(),
        "installed": sorted(available_plugins()),
        "running": {
            "screen": current,
            "plugins": {n: p.available() for n, p in app.plugins.items()},
            "device": {
                "host": app.cfg.host,
                "active": app.device_active,
                "epoch": app.epoch,
                "error": app.last_error,
            },
        },
    }


class Handler(BaseHTTPRequestHandler):
    app: App
    path_cfg: Path

    def log_message(self, fmt, *args):
        log.debug("ui: " + fmt, *args)

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code: int = 200) -> None:
        self._send(code, json.dumps(obj).encode(), "application/json")

    def _body(self) -> dict:
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def do_GET(self):
        route = self.path.split("?", 1)[0]
        if route == "/":
            html = resources.files("smalltv_companion").joinpath("web/index.html").read_bytes()
            self._send(200, html, "text/html; charset=utf-8")
        elif route == "/api/state":
            self._json(state(self.app, self.path_cfg))
        elif route == "/api/fonts":
            self._json(font_families())
        elif route == "/live.png":
            img = self.app.last_frame
            if img is None:
                self._send(HTTPStatus.NO_CONTENT, b"", "image/png")
            else:
                self._send(200, _png(self.app.with_clock(img)), "image/png")
        else:
            self._send(404, b"not found", "text/plain")

    def do_POST(self):
        route = self.path.split("?", 1)[0]
        try:
            body = self._body()
            if route == "/api/render":
                img = render_draft(self.app, body.get("config", {}), body.get("screen"), bool(body.get("notify")))
                self._send(200, _png(img), "image/png")
            elif route == "/api/config":
                cfg = config.save_raw(body.get("config", {}), self.path_cfg)
                self.app.request_reload(cfg)
                self._json({"ok": True, "path": str(self.path_cfg)})
            elif route == "/api/skip":
                self.app.request_skip()
                self._json({"ok": True})
            elif route == "/api/test-notify":
                plugin = self.app.plugins.get("notifications")
                if plugin is None:
                    self._json({"ok": False, "error": "the notifications plugin is not enabled"}, 409)
                    return
                self.app._call(lambda: plugin.add(_sample_note(body.get("text") or "Test notification")))
                self._json({"ok": True})
            else:
                self._send(404, b"not found", "text/plain")
        except (ValueError, KeyError, TypeError) as e:
            self._json({"ok": False, "error": str(e)}, 400)
        except Exception as e:
            log.exception("ui: %s failed", route)
            self._json({"ok": False, "error": str(e)}, 500)


def serve(app: App, path: Path, port: int) -> ThreadingHTTPServer:
    handler = type("BoundHandler", (Handler,), {"app": app, "path_cfg": path})
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, name="webui", daemon=True).start()
    return server
