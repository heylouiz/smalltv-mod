"""config.toml -> the layout the scheduler runs. See config.example.toml."""
from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from .frame import PANEL, Rect
from .render import Theme, parse_color

DEFAULT_PATH = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "smalltv-companion" / "config.toml"


def _rect(v, what: str) -> Rect:
    if not (isinstance(v, (list, tuple)) and len(v) == 4):
        raise ValueError(f"{what}: expected [x, y, w, h]")
    r = Rect(*(int(n) for n in v))
    if r.x < 0 or r.y < 0 or r.w <= 0 or r.h <= 0 or r.x2 > PANEL or r.y2 > PANEL:
        raise ValueError(f"{what}: {list(v)} is not inside the 240x240 panel")
    return r


@dataclass
class ClockConfig:
    enabled: bool = True
    box: Rect = Rect(0, 0, PANEL, 64)
    font: str = "DejaVu Sans:bold"
    size: int = 52
    color: tuple = (255, 255, 255)
    background: tuple | None = None        # None = theme background


@dataclass
class Widget:
    plugin: str
    box: Rect
    options: dict = field(default_factory=dict)


@dataclass
class Screen:
    name: str
    duration: float
    widgets: list[Widget]


@dataclass
class Config:
    host: str = ""
    user: str = "admin"
    password: str = ""
    heartbeat: float = 5.0
    theme: Theme = field(default_factory=Theme)
    clock: ClockConfig = field(default_factory=ClockConfig)
    content: Rect | None = None            # interrupt area; default = below the clock
    screens: list[Screen] = field(default_factory=list)
    plugins: dict[str, dict] = field(default_factory=dict)

    def content_box(self) -> Rect:
        if self.content is not None:
            return self.content
        c = self.clock
        if c.enabled and c.box.x == 0 and c.box.w == PANEL:
            if c.box.y == 0:
                return Rect(0, c.box.y2, PANEL, PANEL - c.box.y2)
            if c.box.y2 == PANEL:
                return Rect(0, 0, PANEL, c.box.y)
        return Rect(0, 0, PANEL, PANEL)

    def used_plugins(self) -> set[str]:
        names = {w.plugin for s in self.screens for w in s.widgets}
        # A plugin configured only to interrupt (notifications) has no widget.
        return names | {n for n, c in self.plugins.items() if c.get("enabled", True)}


def default_screens(content: Rect) -> list[Screen]:
    return [
        Screen("music", 15, [Widget("media", content)]),
        Screen("pc", 10, [Widget("system", content)]),
    ]


def parse(data: dict) -> Config:
    cfg = Config()
    dev = data.get("device", {})
    cfg.host = str(dev.get("host", ""))
    cfg.user = str(dev.get("user", "admin"))
    cfg.password = str(dev.get("password", ""))
    cfg.heartbeat = float(dev.get("heartbeat", cfg.heartbeat))
    cfg.theme = Theme.from_config(data.get("theme", {}))

    c = data.get("clock", {})
    cfg.clock = ClockConfig(
        enabled=bool(c.get("enabled", True)),
        box=_rect(c["box"], "clock.box") if "box" in c else ClockConfig.box,
        font=str(c.get("font", ClockConfig.font)),
        size=int(c.get("size", ClockConfig.size)),
        color=parse_color(c.get("color", "#ffffff")),
        background=parse_color(c["background"]) if "background" in c else None,
    )
    if "content" in data.get("layout", {}):
        cfg.content = _rect(data["layout"]["content"], "layout.content")

    for i, s in enumerate(data.get("screen", [])):
        widgets = []
        for j, w in enumerate(s.get("widget", [])):
            if "plugin" not in w:
                raise ValueError(f"screen[{i}].widget[{j}]: missing plugin")
            box = _rect(w["box"], f"screen[{i}].widget[{j}].box") if "box" in w else cfg.content_box()
            opts = {k: v for k, v in w.items() if k not in ("plugin", "box")}
            widgets.append(Widget(str(w["plugin"]), box, opts))
        cfg.screens.append(Screen(str(s.get("name", f"screen{i}")), float(s.get("duration", 10)), widgets))
    if not cfg.screens:
        cfg.screens = default_screens(cfg.content_box())
    cfg.plugins = {k: dict(v) for k, v in data.get("plugins", {}).items()}
    return cfg


def load(path: Path | None = None) -> Config:
    p = Path(path) if path else DEFAULT_PATH
    if not p.exists():
        return parse({})
    with open(p, "rb") as f:
        return parse(tomllib.load(f))
