import numpy as np

from smalltv_companion.app import App
from smalltv_companion.config import parse
from smalltv_companion.device import NotActive
from smalltv_companion.frame import Rect
from smalltv_companion.plugins.base import Interrupt, Plugin
from smalltv_companion.plugins.media import MediaPlugin, Track, pick, track_from_props
from smalltv_companion.plugins.notifications import NotificationsPlugin, note_from_message


class Fake(Plugin):
    name = "fake"

    def __init__(self, color, avail=True):
        super().__init__({})
        self.color, self.avail, self.pending = color, avail, []

    def available(self):
        return self.avail

    def render(self, ctx):
        ctx.draw.rectangle((0, 0, ctx.w, ctx.h), fill=self.color)

    def interrupt(self):
        return self.pending.pop(0) if self.pending else None


CFG = {
    "clock": {"box": [0, 0, 240, 64], "background": "#000000"},
    "screen": [
        {"name": "a", "duration": 10, "widget": [{"plugin": "a"}]},
        {"name": "b", "duration": 10, "widget": [{"plugin": "b"}]},
    ],
}


def make(a=True, b=True):
    plugins = {"a": Fake((255, 0, 0), a), "b": Fake((0, 0, 255), b)}
    return App(parse(CFG), plugins=plugins), plugins


def px(app, now, x=120, y=150):
    return tuple(np.asarray(app.compose(now))[y, x])


def test_default_widget_box_is_below_the_clock():
    cfg = parse(CFG)
    assert cfg.screens[0].widgets[0].box == Rect(0, 64, 240, 176)


def test_rotation_and_clock_box():
    app, _ = make()
    app.screen_since = 0
    assert px(app, 1) == (255, 0, 0)
    assert px(app, 11) == (0, 0, 255)
    assert px(app, 11, y=10) == (0, 0, 0)          # clock box painted with its bg


def test_unavailable_screens_are_skipped():
    app, _ = make(a=False)
    app.screen_since = 0
    assert px(app, 1) == (0, 0, 255)


def test_interrupt_takes_content_then_returns():
    app, plugins = make()
    app.screen_since = 0
    plugins["b"].pending.append(Interrupt(5, priority=1))
    app.collect_interrupts(1)
    assert px(app, 2) == (0, 0, 255)               # b interrupting during a's slot
    app.collect_interrupts(7)
    assert app.active is None
    assert px(app, 7) == (255, 0, 0)               # a resumes, its slot extended


def test_higher_priority_preempts():
    app, plugins = make()
    plugins["a"].pending.append(Interrupt(5, priority=1))
    app.collect_interrupts(0)
    plugins["b"].pending.append(Interrupt(5, priority=5))
    app.collect_interrupts(1)
    assert app.active.plugin is plugins["b"]


class FakeDevice:
    def __init__(self):
        self.sent, self.epoch, self.active = [], 3, True

    def send_frame_rects(self, arr, rects):
        if not self.active:
            raise NotActive()
        self.sent.append(rects)
        return self.epoch

    def status(self, heartbeat=True):
        from smalltv_companion.device import Status
        return Status(self.epoch, self.active, True, True, {})


def test_push_diffs_and_resends_after_epoch_change():
    app, _ = make()
    dev = FakeDevice()
    app.device = dev
    app.heartbeat()
    frame = np.asarray(app.compose(1))
    assert app.push(frame) == 1
    assert dev.sent[-1] == [Rect(0, 64, 240, 176)]   # full, minus the clock
    assert app.push(frame) == 0                      # nothing changed
    dev.epoch = 4                                    # device repainted (mode switch)
    app.heartbeat()
    assert app.push(frame) == 1
    dev.active = False
    app.shown = None
    assert app.push(frame) == 0 and not app.device_active


def test_mpris_track_parsing():
    class V:
        def __init__(self, v):
            self.value = v
    props = {
        "PlaybackStatus": V("Playing"),
        "Position": V(61_000_000),
        "Metadata": V({"xesam:title": V("Águas de Março"), "xesam:artist": V(["Elis", "Tom"]),
                       "mpris:length": V(212_000_000), "mpris:artUrl": V("file:///x.png")}),
    }
    t = track_from_props("org.mpris.MediaPlayer2.spotify", props)
    assert t == Track("spotify", "Playing", "Águas de Março", "Elis, Tom", "", "file:///x.png", 212, 61)
    paused = Track("vlc", "Paused", "x", "", "", "", 0, 0)
    assert pick([paused, t], True) is t
    assert pick([paused], False) is None


def test_media_render_smoke():
    from PIL import Image
    from smalltv_companion.render import Ctx, Theme
    p = MediaPlugin({})
    p.track = Track("spotify", "Paused", "A very long title that needs wrapping twice over", "Artist", "", "", 200, 50)
    p.render(Ctx(Image.new("RGB", (240, 176)), Theme()))


def test_notification_sources():
    n = note_from_message("org.freedesktop.Notifications", "Notify",
                          ["Slack", 0, "", "<b>Ana</b>", "chego &amp; saio", [], {}, -1])
    assert (n.app, n.summary, n.body) == ("Slack", "Ana", "chego & saio")
    g = note_from_message("org.gtk.Notifications", "AddNotification",
                          ["org.gnome.Calendar", "id", {"title": "Reunião", "body": "em 5 min"}])
    assert (g.app, g.summary) == ("Calendar", "Reunião")
    plugin = NotificationsPlugin({"ignore": ["Spotify"], "duration": 3})
    plugin.add(n)
    plugin.add(note_from_message("org.gtk.Notifications", "AddNotification",
                                 ["notify-send", "1", {"title": "Ana", "body": "chego & saio"}]))  # GNOME echo
    it = plugin.interrupt()
    assert it.duration == 3 and it.key is n and plugin.interrupt() is None
    assert not plugin.accept(note_from_message("org.freedesktop.Notifications", "Notify",
                                               ["Spotify", 0, "", "song", "", [], {}, -1]))
