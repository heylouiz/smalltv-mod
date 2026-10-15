import asyncio
import json
import urllib.request

from smalltv_companion import config
from smalltv_companion.app import App
from smalltv_companion.config import parse
from smalltv_companion.plugins.base import Plugin
from smalltv_companion.webui import render_draft, serve


class Fake(Plugin):
    name = "fake"
    interval = 0

    def __init__(self, config=None):
        super().__init__(config)
        self.stopped = False

    def render(self, ctx):
        ctx.draw.rectangle((0, 0, ctx.w, ctx.h), fill=(255, 0, 0))

    async def stop(self):
        self.stopped = True


RAW = {
    "device": {"host": "10.0.0.1"},
    "theme": {"background": "#000000"},
    "clock": {"box": [0, 0, 240, 64]},
    "screen": [{"name": "a", "duration": 5, "widget": [{"plugin": "a", "box": [0, 64, 240, 176]}]}],
    "plugins": {"a": {"interval": 0}},
}


def test_save_raw_round_trip(tmp_path):
    path = tmp_path / "config.toml"
    cfg = config.save_raw(RAW, path)
    assert cfg.host == "10.0.0.1"
    assert config.load_raw(path) == RAW
    assert path.read_text().startswith("# smalltv-companion config")


def test_save_raw_rejects_bad_box(tmp_path):
    bad = {**RAW, "clock": {"box": [0, 0, 300, 64]}}
    try:
        config.save_raw(bad, tmp_path / "c.toml")
    except ValueError:
        pass
    else:
        raise AssertionError("expected ValueError")
    assert not (tmp_path / "c.toml").exists()


def test_apply_keeps_unchanged_plugins():
    a, b = Fake({"interval": 0}), Fake({"interval": 0})
    app = App(parse(RAW), plugins={"a": a, "b": b})

    async def go():
        new = parse({**RAW, "theme": {"background": "#ffffff"}})
        return await app._apply(new)

    clock_changed = asyncio.run(go())
    assert app.plugins["a"] is a and not a.stopped      # same settings: kept, state intact
    assert "b" not in app.plugins and b.stopped         # no longer used: stopped
    assert clock_changed                                # clock background follows the theme
    assert app.shown is None


def test_render_draft_uses_the_draft_theme():
    app = App(parse(RAW), plugins={"a": Fake()})
    img = render_draft(app, {**RAW, "clock": {"enabled": False}, "theme": {"background": "#0000ff"}})
    assert img.getpixel((5, 5)) == (0, 0, 255)          # no clock: the draft background shows
    assert img.getpixel((5, 100)) == (255, 0, 0)        # the widget, drawn by the running plugin


def test_http_state_and_save(tmp_path):
    path = tmp_path / "config.toml"
    config.save_raw(RAW, path)
    app = App(parse(RAW), plugins={"a": Fake()})
    server = serve(app, path, 0)
    base = f"http://127.0.0.1:{server.server_address[1]}"
    try:
        st = json.load(urllib.request.urlopen(base + "/api/state"))
        assert st["config"] == RAW and "background" in st["defaults"]["theme"]

        new = {**RAW, "plugins": {"a": {"interval": 0}, "notifications": {"duration": 3}}}
        req = urllib.request.Request(base + "/api/config", json.dumps({"config": new}).encode(),
                                     {"Content-Type": "application/json"})
        assert json.load(urllib.request.urlopen(req))["ok"]
        assert config.load_raw(path)["plugins"]["notifications"]["duration"] == 3
        assert app._pending is not None                 # handed to the running loop

        req = urllib.request.Request(base + "/api/render", json.dumps({"config": RAW}).encode(),
                                     {"Content-Type": "application/json"})
        assert urllib.request.urlopen(req).read()[:4] == b"\x89PNG"
    finally:
        server.shutdown()
