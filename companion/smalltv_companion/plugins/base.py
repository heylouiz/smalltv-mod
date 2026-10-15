"""The plugin contract.

A plugin owns one source of information (a music player, the notification
bus, an API) and knows how to draw it into a box. The scheduler:

  1. calls `await start()` once, with the plugin's own config table
     ([plugins.<name>] in config.toml) already in `self.config`;
  2. calls `await poll()` every `interval` seconds (0 = never: event-driven
     plugins call `self.changed()` from their own callbacks instead);
     poll() returns True when something visible changed;
  3. calls `render(ctx)` whenever it composes a screen showing the plugin,
     with `ctx.image` exactly the size of the widget box;
  4. calls `interrupt()` after every poll/changed(): returning an Interrupt
     takes over the whole content area (everything but the clock) for its
     duration, highest priority first. Notifications use this.

`available()` False hides the plugin: a screen whose widgets are all
unavailable is skipped in the rotation (no music playing -> no music screen).
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Callable

from ..render import Ctx


@dataclass
class Interrupt:
    duration: float                  # seconds on screen
    priority: int = 0                # higher wins; ties queue in arrival order
    key: object = None               # passed back to render_interrupt()


class Plugin:
    name = "plugin"
    interval: float = 5.0

    def __init__(self, config: dict | None = None):
        self.config = config or {}
        self.interval = float(self.config.get("interval", self.interval))
        self._notify: Callable[[], None] = lambda: None

    # -- lifecycle -------------------------------------------------------
    async def start(self) -> None:
        pass

    async def stop(self) -> None:
        pass

    async def poll(self) -> bool:
        return False

    # -- drawing ---------------------------------------------------------
    def available(self) -> bool:
        return True

    def render(self, ctx: Ctx) -> None:
        raise NotImplementedError

    # -- interruptions (optional) ----------------------------------------
    def interrupt(self) -> Interrupt | None:
        return None

    def render_interrupt(self, ctx: Ctx, key: object) -> None:
        self.render(ctx)

    # -- for event-driven plugins ------------------------------------------
    def changed(self) -> None:
        """Ask the scheduler to recompose now (safe from the event loop)."""
        self._notify()
