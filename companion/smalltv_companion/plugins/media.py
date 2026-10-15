"""What is playing, from any MPRIS player on the session bus (Spotify, the
browser, mpv, VLC...): cover art, title, artist and progress.

    [plugins.media]
    interval = 1
    ignore = ["chromium"]     # player bus-name fragments to skip
    show_paused = true         # keep showing a paused track
"""
from __future__ import annotations

import asyncio
import io
import logging
from dataclasses import dataclass
from urllib.parse import unquote, urlparse

import requests
from PIL import Image

from ..render import Ctx, cover
from .base import Plugin

log = logging.getLogger(__name__)
MPRIS = "org.mpris.MediaPlayer2."


@dataclass(frozen=True)
class Track:
    player: str
    status: str            # Playing / Paused / Stopped
    title: str
    artist: str
    album: str
    art_url: str
    length_s: int          # 0 = unknown
    position_s: int


def _unwrap(v):
    return getattr(v, "value", v)


def track_from_props(player: str, props: dict, position_us: int | None = None) -> Track | None:
    meta = {k: _unwrap(v) for k, v in (_unwrap(props.get("Metadata")) or {}).items()}
    title = str(meta.get("xesam:title") or "")
    if not title:
        return None
    artist = meta.get("xesam:artist") or []
    if isinstance(artist, str):
        artist = [artist]
    pos = position_us if position_us is not None else _unwrap(props.get("Position")) or 0
    return Track(
        player=player.removeprefix(MPRIS),
        status=str(_unwrap(props.get("PlaybackStatus")) or "Stopped"),
        title=title,
        artist=", ".join(str(a) for a in artist),
        album=str(meta.get("xesam:album") or ""),
        art_url=str(meta.get("mpris:artUrl") or ""),
        length_s=int(meta.get("mpris:length") or 0) // 1_000_000,
        position_s=int(pos) // 1_000_000,
    )


def pick(tracks: list[Track], show_paused: bool) -> Track | None:
    for t in tracks:
        if t.status == "Playing":
            return t
    if show_paused:
        for t in tracks:
            if t.status == "Paused":
                return t
    return None


def mmss(s: int) -> str:
    return f"{s // 60}:{s % 60:02d}"


class MediaPlugin(Plugin):
    name = "media"
    interval = 1.0

    def __init__(self, config=None):
        super().__init__(config)
        self.bus = None
        self.track: Track | None = None
        self._art: dict[str, Image.Image | None] = {}

    async def start(self) -> None:
        from dbus_next import BusType
        from dbus_next.aio import MessageBus
        self.bus = await MessageBus(bus_type=BusType.SESSION).connect()

    async def stop(self) -> None:
        if self.bus:
            self.bus.disconnect()

    async def _call(self, dest, path, iface, member, sig="", body=()):
        from dbus_next import Message, MessageType
        reply = await self.bus.call(Message(destination=dest, path=path, interface=iface,
                                            member=member, signature=sig, body=list(body)))
        if reply.message_type == MessageType.ERROR:
            raise RuntimeError(reply.error_name)
        return reply.body

    async def _tracks(self) -> list[Track]:
        names = (await self._call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                  "org.freedesktop.DBus", "ListNames"))[0]
        ignore = [str(i).lower() for i in self.config.get("ignore", [])]
        out = []
        for n in sorted(n for n in names if n.startswith(MPRIS)):
            if any(i in n.lower() for i in ignore):
                continue
            try:
                props = (await self._call(n, "/org/mpris/MediaPlayer2", "org.freedesktop.DBus.Properties",
                                          "GetAll", "s", ["org.mpris.MediaPlayer2.Player"]))[0]
            except Exception:
                continue
            t = track_from_props(n, props)
            if t:
                out.append(t)
        return out

    async def poll(self) -> bool:
        if not self.bus:
            return False
        track = pick(await self._tracks(), bool(self.config.get("show_paused", True)))
        if track and track.art_url and track.art_url not in self._art:
            self._art[track.art_url] = await asyncio.to_thread(self._load_art, track.art_url)
            if len(self._art) > 8:
                self._art.pop(next(iter(self._art)))
        changed, self.track = track != self.track, track
        return changed

    @staticmethod
    def _load_art(url: str) -> Image.Image | None:
        try:
            u = urlparse(url)
            if u.scheme == "file":
                img = Image.open(unquote(u.path))
            elif u.scheme in ("http", "https"):
                img = Image.open(io.BytesIO(requests.get(url, timeout=10).content))
            else:
                return None
            img.load()
            return img.convert("RGB")
        except Exception as e:
            log.debug("cover art %s: %s", url, e)
            return None

    def available(self) -> bool:
        return self.track is not None

    def render(self, ctx: Ctx) -> None:
        t = self.track
        if t is None:
            return
        pad = 12
        art_size = min(104, ctx.h - 64)
        art = self._art.get(t.art_url)
        if art is not None:
            ctx.image.paste(cover(art, (art_size, art_size)), (pad, pad))
        else:
            ctx.draw.rounded_rectangle((pad, pad, pad + art_size, pad + art_size), 10, fill=ctx.theme.panel)
            ctx.text((pad + art_size // 2, pad + art_size // 2), "♪", 48, fill=ctx.theme.muted, anchor="mm")

        x = pad + art_size + 10
        tw = ctx.w - x - pad
        title_font = ctx.font(18, bold=True)
        lines = ctx.wrap(t.title, title_font, tw, 3)
        y = pad
        for line in lines:
            ctx.draw.text((x, y), line, font=title_font, fill=ctx.theme.foreground)
            y += 22
        if t.artist:
            artist_font = ctx.font(15)
            for line in ctx.wrap(t.artist, artist_font, tw, 2):
                ctx.draw.text((x, y + 4), line, font=artist_font, fill=ctx.theme.muted)
                y += 18

        bar_y = ctx.h - 36
        if t.length_s:
            ctx.bar((pad, bar_y, ctx.w - pad, bar_y + 8), t.position_s / t.length_s)
            ctx.text((pad, bar_y + 14), mmss(t.position_s), 14, fill=ctx.theme.muted)
            ctx.text((ctx.w - pad, bar_y + 14), mmss(t.length_s), 14, fill=ctx.theme.muted, anchor="ra")
        if t.status != "Playing":
            ctx.text((ctx.w // 2, bar_y + 14), "PAUSED", 14, fill=ctx.theme.accent, bold=True, anchor="ma")
        elif not t.length_s:
            ctx.text((ctx.w // 2, bar_y + 14), t.player, 14, fill=ctx.theme.muted, anchor="ma")
