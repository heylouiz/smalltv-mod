"""Fixed text, or the output of a shell command (the simplest plugin; also
a template for writing your own).

    [plugins.text]
    command = "uptime -p"      # optional; without it, `text` is shown
    text = "Olá!"
    interval = 30
"""
from __future__ import annotations

import asyncio

from ..render import Ctx
from .base import Plugin


class TextPlugin(Plugin):
    name = "text"
    interval = 30.0

    def __init__(self, config=None):
        super().__init__(config)
        self.value = str(self.config.get("text", ""))

    async def poll(self) -> bool:
        cmd = self.config.get("command")
        if not cmd:
            return False
        proc = await asyncio.create_subprocess_shell(cmd, stdout=asyncio.subprocess.PIPE,
                                                     stderr=asyncio.subprocess.DEVNULL)
        out, _ = await proc.communicate()
        value = out.decode(errors="replace").strip()
        changed, self.value = value != self.value, value
        return changed

    def available(self) -> bool:
        return bool(self.value)

    def render(self, ctx: Ctx) -> None:
        size = int(ctx.options.get("size", 24))
        title = ctx.options.get("title")
        y = 12
        if title:
            ctx.text((ctx.w // 2, y), str(title), 14, fill=ctx.theme.muted, anchor="mt", max_w=ctx.w - 16)
            y += 26
        font = ctx.font(size, bold=True)
        lines = ctx.wrap(self.value, font, ctx.w - 20, max(1, (ctx.h - y) // int(size * 1.25)))
        total = len(lines) * int(size * 1.25)
        y = max(y, (ctx.h - total) // 2)
        for line in lines:
            ctx.draw.text((ctx.w // 2, y), line, font=font, fill=ctx.theme.foreground, anchor="mt")
            y += int(size * 1.25)
