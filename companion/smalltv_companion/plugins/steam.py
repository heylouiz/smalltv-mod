"""Steam: the game you are playing (with its header art), or which friends
are in a game. Uses the Steam Web API; get a key at
https://steamcommunity.com/dev/apikey and your 64-bit SteamID from your
profile URL. Your profile's game details must be public (or friends-only
for friends' games to show).

    [plugins.steam]
    api_key = "XXXXXXXXXXXXXXXX"
    steam_id = "7656119..."
    interval = 60
    friends = true
"""
from __future__ import annotations

import asyncio
import io
import logging
from dataclasses import dataclass

import requests
from PIL import Image

from ..render import Ctx, cover
from .base import Plugin

log = logging.getLogger(__name__)
API = "https://api.steampowered.com"
HEADER = "https://cdn.cloudflare.steamstatic.com/steam/apps/{appid}/header.jpg"


@dataclass(frozen=True)
class Player:
    name: str
    game: str
    appid: str


def players_from_summaries(data: dict) -> list[Player]:
    out = []
    for p in data.get("response", {}).get("players", []):
        out.append(Player(p.get("personaname", "?"), p.get("gameextrainfo", ""), str(p.get("gameid", ""))))
    return out


class SteamPlugin(Plugin):
    name = "steam"
    interval = 60.0

    def __init__(self, config=None):
        super().__init__(config)
        self.key = self.config.get("api_key", "")
        self.steam_id = str(self.config.get("steam_id", ""))
        self.me: Player | None = None
        self.friends_playing: list[Player] = []
        self._headers: dict[str, Image.Image | None] = {}
        self._friend_ids: list[str] = []

    def _get(self, path: str, **params) -> dict:
        r = requests.get(f"{API}/{path}", params={"key": self.key, **params}, timeout=15)
        r.raise_for_status()
        return r.json()

    def _fetch(self):
        me = players_from_summaries(self._get("ISteamUser/GetPlayerSummaries/v0002/", steamids=self.steam_id))
        friends: list[Player] = []
        if self.config.get("friends", True):
            if not self._friend_ids:
                try:
                    fl = self._get("ISteamUser/GetFriendList/v0001/", steamid=self.steam_id, relationship="friend")
                    self._friend_ids = [f["steamid"] for f in fl.get("friendslist", {}).get("friends", [])][:100]
                except requests.HTTPError as e:   # 401 when the friend list is private
                    log.info("steam friend list unavailable: %s", e)
            if self._friend_ids:
                summ = self._get("ISteamUser/GetPlayerSummaries/v0002/", steamids=",".join(self._friend_ids))
                friends = sorted((p for p in players_from_summaries(summ) if p.game), key=lambda p: p.name.lower())
        header = None
        game = me[0] if me and me[0].game else None
        if game and game.appid not in self._headers:
            try:
                header = Image.open(io.BytesIO(requests.get(HEADER.format(appid=game.appid), timeout=15).content))
                header.load()
            except Exception:
                header = None
            self._headers[game.appid] = header
        return (me[0] if me else None), friends

    async def poll(self) -> bool:
        if not (self.key and self.steam_id):
            return False
        me, friends = await asyncio.to_thread(self._fetch)
        changed = (me, friends) != (self.me, self.friends_playing)
        self.me, self.friends_playing = me, friends
        return changed

    def available(self) -> bool:
        return bool((self.me and self.me.game) or self.friends_playing)

    def render(self, ctx: Ctx) -> None:
        pad = 12
        if self.me and self.me.game:
            header = self._headers.get(self.me.appid)
            hh = min(ctx.h // 2, round((ctx.w - 2 * pad) * 215 / 460))
            if header is not None:
                ctx.image.paste(cover(header, (ctx.w - 2 * pad, hh)), (pad, pad))
            y = pad + hh + 10
            ctx.text((pad, y), "PLAYING NOW", 12, fill=ctx.theme.accent, bold=True)
            ctx.text((pad, y + 16), self.me.game, 20, bold=True, max_w=ctx.w - 2 * pad, min_size=13)
            y += 46
            others = [f for f in self.friends_playing if f.game == self.me.game]
            if others:
                names = ", ".join(f.name for f in others)
                ctx.text((pad, y), f"with {names}", 14, fill=ctx.theme.muted, max_w=ctx.w - 2 * pad, min_size=11)
            return
        ctx.text((pad, pad), "FRIENDS IN GAME", 12, fill=ctx.theme.accent, bold=True)
        y = pad + 22
        for f in self.friends_playing[: max(1, (ctx.h - y) // 36)]:
            ctx.text((pad, y), f.name, 15, bold=True, max_w=ctx.w - 2 * pad, min_size=11)
            ctx.text((pad, y + 17), f.game, 13, fill=ctx.theme.muted, max_w=ctx.w - 2 * pad, min_size=10)
            y += 36
