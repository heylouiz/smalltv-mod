"""Desktop notifications, shown on the SmallTV as they pop up on the PC.

Listens to the session bus as a monitor (no notification daemon is
replaced): freedesktop Notify calls (most apps, Electron, Chrome, Firefox,
notify-send), GTK's org.gtk.Notifications (GNOME apps) and the flatpak
portal. Each one interrupts the rotation for `duration` seconds; as a widget
it lists the most recent ones.

    [plugins.notifications]
    duration = 8
    ignore = ["Spotify"]        # app names to drop (case-insensitive)
    only = []                    # if set, only these apps
"""
from __future__ import annotations

import collections
import html
import logging
import os
import re
import time
from dataclasses import dataclass, field

from PIL import Image

from ..render import Ctx, cover
from .base import Interrupt, Plugin

log = logging.getLogger(__name__)

RULES = [
    "type='method_call',interface='org.freedesktop.Notifications',member='Notify'",
    "type='method_call',interface='org.gtk.Notifications',member='AddNotification'",
    "type='method_call',interface='org.freedesktop.portal.Notification',member='AddNotification'",
]


@dataclass
class Note:
    app: str
    summary: str
    body: str
    icon: str = ""
    urgency: int = 1
    at: float = field(default_factory=time.time)


def _v(x):
    return getattr(x, "value", x)


def clean(text: str) -> str:
    return html.unescape(re.sub(r"<[^>]+>", "", text or "")).strip()


def note_from_message(interface: str, member: str, body: list) -> Note | None:
    if interface == "org.freedesktop.Notifications" and member == "Notify" and len(body) >= 5:
        app, _replaces, icon, summary, text = body[:5]
        hints = body[6] if len(body) > 6 else {}
        urgency = int(_v(hints.get("urgency", 1))) if isinstance(hints, dict) else 1
        image = _v(hints.get("image-path", "")) if isinstance(hints, dict) else ""
        return Note(str(app), clean(summary), clean(text), str(image or icon or ""), urgency)
    if member == "AddNotification":
        if interface == "org.gtk.Notifications" and len(body) >= 3:
            app, data = str(body[0]).rsplit(".", 1)[-1], body[2]
        elif len(body) >= 2:
            app, data = "", body[1]
        else:
            return None
        data = {k: _v(v) for k, v in data.items()}
        prio = {"urgent": 2, "high": 2, "low": 0}.get(str(data.get("priority", "")), 1)
        return Note(app, clean(str(data.get("title", ""))), clean(str(data.get("body", ""))), "", prio)
    return None


class NotificationsPlugin(Plugin):
    name = "notifications"
    interval = 0          # event-driven

    def __init__(self, config=None):
        super().__init__(config)
        self.duration = float(self.config.get("duration", 8))
        self.ignore = {a.lower() for a in self.config.get("ignore", [])}
        self.only = {a.lower() for a in self.config.get("only", [])}
        self.recent: collections.deque[Note] = collections.deque(maxlen=5)
        self.pending: collections.deque[Note] = collections.deque(maxlen=10)
        self.bus = None

    async def start(self) -> None:
        from dbus_next import BusType, Message
        from dbus_next.aio import MessageBus
        self.bus = await MessageBus(bus_type=BusType.SESSION).connect()
        self.bus.add_message_handler(self._on_message)
        reply = await self.bus.call(Message(
            destination="org.freedesktop.DBus", path="/org/freedesktop/DBus",
            interface="org.freedesktop.DBus.Monitoring", member="BecomeMonitor",
            signature="asu", body=[RULES, 0]))
        if reply.error_name:
            raise RuntimeError(f"BecomeMonitor: {reply.error_name}")

    async def stop(self) -> None:
        if self.bus:
            self.bus.disconnect()

    def _on_message(self, msg):
        from dbus_next import MessageType
        # Replies (BecomeMonitor's own included) must reach dbus-next's
        # reply matching, so only method calls are ours. Those are claimed
        # even when ignored: a monitor connection must never send anything
        # (the bus drops it), and dbus-next would otherwise answer an
        # unhandled call with an error reply.
        if msg.message_type != MessageType.METHOD_CALL:
            return None
        try:
            note = note_from_message(msg.interface or "", msg.member or "", list(msg.body or []))
        except Exception:
            log.exception("could not read notification")
            return True
        if note and self.accept(note):
            self.add(note)
        return True

    def accept(self, note: Note) -> bool:
        app = note.app.lower()
        if self.only and app not in self.only:
            return False
        return app not in self.ignore and bool(note.summary or note.body)

    def add(self, note: Note) -> None:
        # GNOME's freedesktop daemon is a proxy that re-sends each Notify to
        # gnome-shell as org.gtk.Notifications.AddNotification, so the monitor
        # sees every notification twice.
        last = self.recent[0] if self.recent else None
        if last and (last.summary, last.body) == (note.summary, note.body) and note.at - last.at < 3:
            return
        self.recent.appendleft(note)
        self.pending.append(note)
        self.changed()

    def interrupt(self) -> Interrupt | None:
        if not self.pending:
            return None
        note = self.pending.popleft()
        return Interrupt(self.duration, priority=10 + note.urgency, key=note)

    def available(self) -> bool:
        return bool(self.recent)

    def _icon(self, note: Note, size: int) -> Image.Image | None:
        path = note.icon.removeprefix("file://")
        if path and os.path.isabs(path) and os.path.isfile(path):
            try:
                return cover(Image.open(path), (size, size))
            except Exception:
                return None
        return None

    def render_interrupt(self, ctx: Ctx, note: Note) -> None:
        pad = 14
        accent = (230, 80, 60) if note.urgency >= 2 else ctx.theme.accent
        ctx.draw.rectangle((0, 0, 5, ctx.h), fill=accent)
        y = pad
        icon = self._icon(note, 36)
        x = pad + 4
        if icon is not None:
            ctx.image.paste(icon, (x, y))
            x += 46
        ctx.text((x, y + 2), note.app or "Notification", 15, fill=ctx.theme.muted, bold=True,
                 max_w=ctx.w - x - pad, min_size=12)
        y += 44
        title_font = ctx.font(20, bold=True)
        for line in ctx.wrap(note.summary, title_font, ctx.w - 2 * pad - 4, 2):
            ctx.draw.text((pad + 4, y), line, font=title_font, fill=ctx.theme.foreground)
            y += 25
        body_font = ctx.font(16)
        max_lines = max(0, (ctx.h - y - pad) // 20)
        for line in ctx.wrap(note.body, body_font, ctx.w - 2 * pad - 4, max_lines):
            ctx.draw.text((pad + 4, y + 4), line, font=body_font, fill=ctx.theme.foreground)
            y += 20

    def render(self, ctx: Ctx) -> None:
        pad, y = 12, 10
        for note in list(self.recent)[:3]:
            ago = int(time.time() - note.at) // 60
            ctx.text((pad, y), f"{note.app or 'app'} · {ago} min" if ago else (note.app or "app"),
                     12, fill=ctx.theme.muted, max_w=ctx.w - 2 * pad)
            ctx.text((pad, y + 16), note.summary or note.body, 16, bold=True, max_w=ctx.w - 2 * pad, min_size=12)
            y += 52
