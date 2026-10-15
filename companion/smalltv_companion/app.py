"""The scheduler: runs the plugins, composes the screen, pushes what changed.

One asyncio loop. Plugins poll on their own interval (or call changed());
either way the loop wakes, composes a 240x240 frame, diffs it against what
the device already shows and pushes only the changed rectangles. A heartbeat
(GET status?hb=1) keeps the device from showing its offline screen while
nothing changes, and reveals when the device painted over our frame (its
epoch moved: reboot, mode switch, settings save) so the next push is full.
"""
from __future__ import annotations

import asyncio
import logging
import time
from dataclasses import dataclass

import numpy as np
from PIL import Image

from . import glyphs
from .config import Config, Screen
from .device import Device, NotActive
from .frame import PANEL, Rect, color565, diff, to_array
from .plugins import Interrupt, Plugin, plugin_class
from .render import Ctx, load_font

log = logging.getLogger(__name__)


@dataclass
class _Active:
    plugin: Plugin
    interrupt: Interrupt
    until: float


class App:
    def __init__(self, cfg: Config, device: Device | None = None, plugins: dict[str, Plugin] | None = None):
        self.cfg = cfg
        self.device = device
        self.plugins: dict[str, Plugin] = plugins if plugins is not None else self._make_plugins()
        self.clock_bg = cfg.clock.background or cfg.theme.background
        self.screen_idx = 0
        self.screen_since = time.monotonic()
        self.queue: list[tuple[int, int, Plugin, Interrupt]] = []   # (-priority, seq, ...)
        self.active: _Active | None = None
        self._seq = 0
        self.shown: np.ndarray | None = None    # what the device shows now (None = unknown)
        self.epoch: int | None = None
        self.device_active = False
        self.last_error = ""
        self.last_frame: Image.Image | None = None   # the newest composed frame (for the web UI)
        self._dirty: asyncio.Event | None = None
        self._loop: asyncio.AbstractEventLoop | None = None
        self._tasks: dict[str, asyncio.Task] = {}
        self._pending: Config | None = None

    # -- plugins -----------------------------------------------------------
    def _make_plugins(self, names=None) -> dict[str, Plugin]:
        out = {}
        for name in sorted(self.cfg.used_plugins() if names is None else names):
            try:
                out[name] = plugin_class(name)(self.cfg.plugins.get(name, {}))
            except Exception as e:  # a broken plugin must not take the others down
                log.error("plugin %s disabled: %s", name, e)
        return out

    async def _start_plugin(self, p: Plugin) -> None:
        loop = asyncio.get_running_loop()
        p._notify = lambda: loop.call_soon_threadsafe(self._mark_dirty)
        try:
            await p.start()
        except Exception as e:
            log.error("plugin %s failed to start: %s", p.name, e)
        if p.interval > 0:
            self._tasks[p.name] = asyncio.create_task(self._poll_loop(p))

    async def _stop_plugin(self, name: str) -> None:
        task = self._tasks.pop(name, None)
        if task:
            task.cancel()
        try:
            await self.plugins[name].stop()
        except Exception:
            pass

    # -- live changes (called from other threads: the web UI) -----------------
    def _call(self, fn) -> None:
        if self._loop is None:
            fn()
        else:
            self._loop.call_soon_threadsafe(fn)

    def request_reload(self, cfg: Config) -> None:
        """Applies a new config at the next loop iteration, without a restart."""
        def go():
            self._pending = cfg
            self._mark_dirty()
        self._call(go)

    def request_skip(self) -> None:
        """Moves the rotation on to the next screen (or ends an interruption)."""
        def go():
            if self.active:
                self.active = None
            else:
                self.screen_idx += 1
                self.screen_since = time.monotonic()
            self._mark_dirty()
        self._call(go)

    async def _apply(self, cfg: Config) -> bool:
        """Swaps in `cfg`; plugins whose settings did not change keep running
        (and keep their state: recent notifications, the current track).
        Returns True when the clock has to be set up on the device again."""
        old, self.cfg = self.cfg, cfg
        wanted = cfg.used_plugins()
        for name in list(self.plugins):
            if name not in wanted or old.plugins.get(name, {}) != cfg.plugins.get(name, {}):
                await self._stop_plugin(name)
                del self.plugins[name]
        fresh = self._make_plugins(wanted - set(self.plugins))
        self.plugins.update(fresh)
        for p in fresh.values():
            await self._start_plugin(p)
        if self.active and self.plugins.get(self.active.plugin.name) is not self.active.plugin:
            self.active = None
        self.screen_idx, self.screen_since = 0, time.monotonic()
        new_bg = cfg.clock.background or cfg.theme.background
        clock_changed = old.clock != cfg.clock or new_bg != self.clock_bg
        self.clock_bg = new_bg
        if clock_changed:
            self.shown = None
        return clock_changed

    def _mark_dirty(self) -> None:
        if self._dirty is not None:
            self._dirty.set()

    async def _poll_loop(self, p: Plugin) -> None:
        while True:
            try:
                if await p.poll():
                    self._mark_dirty()
            except Exception:
                log.exception("plugin %s: poll failed", p.name)
            await asyncio.sleep(max(0.2, p.interval))

    # -- screens and interruptions ------------------------------------------
    def visible_screens(self) -> list[Screen]:
        return [s for s in self.cfg.screens
                if any((p := self.plugins.get(w.plugin)) and p.available() for w in s.widgets)]

    def current_screen(self, now: float) -> Screen | None:
        screens = self.visible_screens()
        if not screens:
            return None
        if self.screen_idx >= len(screens):
            self.screen_idx = 0
        if now - self.screen_since >= screens[self.screen_idx].duration:
            self.screen_idx = (self.screen_idx + 1) % len(screens)
            self.screen_since = now
        return screens[self.screen_idx]

    def collect_interrupts(self, now: float) -> None:
        for p in self.plugins.values():
            try:
                it = p.interrupt()
            except Exception:
                log.exception("plugin %s: interrupt failed", p.name)
                continue
            if it is not None:
                self._seq += 1
                self.queue.append((-it.priority, self._seq, p, it))
        self.queue.sort(key=lambda e: (e[0], e[1]))
        if self.active and now >= self.active.until:
            self.active = None
        if self.queue and (self.active is None or -self.queue[0][0] > self.active.interrupt.priority):
            _, _, p, it = self.queue.pop(0)
            self.active = _Active(p, it, now + it.duration)
            self.screen_since += it.duration    # the rotation resumes where it was

    def next_deadline(self, now: float) -> float:
        times = [now + self.cfg.heartbeat]
        if self.active:
            times.append(self.active.until)
        screens = self.visible_screens()
        if len(screens) > 1 and self.screen_idx < len(screens):
            times.append(self.screen_since + screens[self.screen_idx].duration)
        return min(times)

    # -- composing ------------------------------------------------------------
    def compose(self, now: float | None = None) -> Image.Image:
        now = time.monotonic() if now is None else now
        theme = self.cfg.theme
        img = Image.new("RGB", (PANEL, PANEL), theme.background)
        if self.active:
            box = self.cfg.content_box()
            self._render(img, box, lambda ctx: self.active.plugin.render_interrupt(ctx, self.active.interrupt.key), {})
        else:
            screen = self.current_screen(now)
            if screen is None:
                self._render(img, self.cfg.content_box(), self._render_idle, {})
            else:
                for w in screen.widgets:
                    p = self.plugins.get(w.plugin)
                    if p and p.available():
                        self._render(img, w.box, p.render, w.options)
        if self.cfg.clock.enabled:
            b = self.cfg.clock.box
            img.paste(self.clock_bg, (b.x, b.y, b.x2, b.y2))
        return img

    def _render(self, img: Image.Image, box: Rect, fn, options: dict) -> None:
        ctx = Ctx(Image.new("RGB", (box.w, box.h), self.cfg.theme.background), self.cfg.theme, options)
        try:
            fn(ctx)
        except Exception:
            log.exception("render failed")
            ctx.draw.rectangle((0, 0, box.w - 1, box.h - 1), fill=self.cfg.theme.background)
            ctx.text((box.w // 2, box.h // 2), "render error", 14, fill=self.cfg.theme.muted, anchor="mm")
        img.paste(ctx.image, (box.x, box.y))

    def _render_idle(self, ctx: Ctx) -> None:
        ctx.text((ctx.w // 2, ctx.h // 2), "nothing to show", 16, fill=ctx.theme.muted, anchor="mm")

    def preview(self) -> Image.Image:
        """compose() plus the clock as the device would draw it."""
        return self.with_clock(self.compose())

    def with_clock(self, img: Image.Image) -> Image.Image:
        img = img.copy()
        c = self.cfg.clock
        if c.enabled:
            atlas = glyphs.render_glyphs(load_font(c.font, c.size), c.color, self.clock_bg)
            clock = glyphs.preview(atlas, time.strftime("%H:%M"))
            img.paste(clock, (c.box.x + (c.box.w - clock.width) // 2, c.box.y + (c.box.h - clock.height) // 2))
        return img

    # -- device ------------------------------------------------------------------
    def clock_atlas(self) -> bytes:
        c = self.cfg.clock
        images = glyphs.render_glyphs(load_font(c.font, c.size), c.color, self.clock_bg)
        if glyphs.atlas_width(images) > c.box.w or images[0].height > c.box.h:
            log.warning("clock font size %d does not fit the clock box; the device falls back to its own font", c.size)
        return glyphs.pack_atlas(images)

    def setup_clock(self) -> None:
        c = self.cfg.clock
        if c.enabled:
            self.device.upload_glyphs(self.clock_atlas())
        self.device.set_clock(c.enabled, c.box if c.enabled else None, color565(self.clock_bg), color565(c.color))

    def heartbeat(self) -> None:
        st = self.device.status(heartbeat=True)
        if st.epoch != self.epoch or not st.active:
            self.shown = None            # device painted over us: resend everything
        self.epoch = st.epoch
        self.device_active = st.active

    def push(self, frame: np.ndarray) -> int:
        """Sends what changed; returns the number of rectangles pushed."""
        exclude = self.cfg.clock.box if self.cfg.clock.enabled else None
        rects = diff(self.shown, frame, exclude)
        if not rects:
            return 0
        try:
            self.epoch = self.device.send_frame_rects(frame, rects)
        except NotActive:
            self.device_active = False
            self.shown = None
            return 0
        self.shown = frame
        return len(rects)

    # -- main loop ------------------------------------------------------------------
    async def run(self, once: bool = False) -> None:
        self._dirty = asyncio.Event()
        self._loop = asyncio.get_running_loop()
        for p in list(self.plugins.values()):
            await self._start_plugin(p)
        clock_ready = False
        last_hb = 0.0
        backoff = 1.0
        try:
            while True:
                now = time.monotonic()
                if self._pending is not None:
                    cfg, self._pending = self._pending, None
                    if await self._apply(cfg):
                        clock_ready = False
                try:
                    if not clock_ready:
                        await asyncio.to_thread(self.setup_clock)
                        clock_ready = True
                    if now - last_hb >= self.cfg.heartbeat or self.epoch is None:
                        await asyncio.to_thread(self.heartbeat)
                        last_hb = now
                    self.collect_interrupts(now)
                    img = self.compose(now)
                    self.last_frame = img
                    if self.device_active:
                        frame = to_array(img)
                        t0 = time.monotonic()
                        n = await asyncio.to_thread(self.push, frame)
                        if n:
                            log.debug("pushed %d rect(s) in %.2fs", n, time.monotonic() - t0)
                    backoff = 1.0
                    self.last_error = ""
                except Exception as e:
                    log.warning("device unreachable (%s); retrying in %.0fs", e, backoff)
                    self.last_error = str(e)
                    self.last_frame = self.compose(time.monotonic())
                    self.shown = None
                    self.epoch = None
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 2, 30.0)
                    continue
                if once:
                    return
                timeout = max(0.05, self.next_deadline(time.monotonic()) - time.monotonic())
                try:
                    await asyncio.wait_for(self._dirty.wait(), timeout)
                except asyncio.TimeoutError:
                    pass
                self._dirty.clear()
        finally:
            for name in list(self.plugins):
                await self._stop_plugin(name)
