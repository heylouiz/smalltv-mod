"""What a plugin draws with: a box-sized image, the theme colours, fonts by
name, and a few text helpers that every widget ends up needing."""
from __future__ import annotations

import functools
import os
import shutil
import subprocess
from dataclasses import dataclass, field

from PIL import Image, ImageColor, ImageDraw, ImageFont


def parse_color(value) -> tuple[int, int, int]:
    if isinstance(value, (tuple, list)):
        return tuple(int(v) for v in value[:3])
    return ImageColor.getrgb(str(value))[:3]


@functools.lru_cache(maxsize=64)
def resolve_font_path(spec: str) -> str:
    """A path, or a fontconfig pattern such as "Inter:bold" / "DejaVu Sans"."""
    if os.path.isfile(os.path.expanduser(spec)):
        return os.path.expanduser(spec)
    if shutil.which("fc-match"):
        out = subprocess.run(["fc-match", "-f", "%{file}", spec], capture_output=True, text=True)
        if out.returncode == 0 and out.stdout:
            return out.stdout
    raise FileNotFoundError(f"font not found: {spec}")


@functools.lru_cache(maxsize=128)
def load_font(spec: str, size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(resolve_font_path(spec), size)


@dataclass
class Theme:
    background: tuple = (16, 24, 32)
    foreground: tuple = (240, 240, 240)
    muted: tuple = (138, 150, 163)
    accent: tuple = (29, 185, 84)
    panel: tuple = (34, 44, 56)
    font: str = "DejaVu Sans"
    font_bold: str = "DejaVu Sans:bold"

    @classmethod
    def from_config(cls, cfg: dict) -> "Theme":
        t = cls()
        for key in ("background", "foreground", "muted", "accent", "panel"):
            if key in cfg:
                setattr(t, key, parse_color(cfg[key]))
        t.font = cfg.get("font", t.font)
        t.font_bold = cfg.get("font_bold", t.font_bold)
        return t


@dataclass
class Ctx:
    """Handed to Plugin.render(): draw into `image` (exactly the widget box)."""
    image: Image.Image
    theme: Theme
    options: dict = field(default_factory=dict)

    def __post_init__(self):
        self.draw = ImageDraw.Draw(self.image)

    @property
    def w(self) -> int:
        return self.image.width

    @property
    def h(self) -> int:
        return self.image.height

    def font(self, size: int, bold: bool = False) -> ImageFont.FreeTypeFont:
        return load_font(self.theme.font_bold if bold else self.theme.font, size)

    def fit(self, text: str, max_w: int, size: int, bold: bool = False, min_size: int = 10):
        """Largest font <= size that fits `text` in max_w, else the smallest
        one with the text ellipsized."""
        for s in range(size, min_size - 1, -1):
            f = self.font(s, bold)
            if f.getlength(text) <= max_w:
                return f, text
        f = self.font(min_size, bold)
        return f, ellipsize(text, f, max_w)

    def text(self, xy, text: str, size: int, fill=None, bold: bool = False,
             max_w: int | None = None, anchor: str = "la", min_size: int | None = None):
        fill = self.theme.foreground if fill is None else fill
        if max_w is None:
            f = self.font(size, bold)
        else:
            f, text = self.fit(text, max_w, size, bold, min_size or size)
        self.draw.text(xy, text, font=f, fill=fill, anchor=anchor)
        return f

    def bar(self, box, fraction: float, fill=None, track=None, radius: int | None = None):
        x0, y0, x1, y1 = box
        r = (y1 - y0) // 2 if radius is None else radius
        self.draw.rounded_rectangle(box, r, fill=self.theme.panel if track is None else track)
        f = max(0.0, min(1.0, fraction))
        if f > 0:
            self.draw.rounded_rectangle((x0, y0, x0 + max(y1 - y0, int((x1 - x0) * f)), y1), r,
                                        fill=self.theme.accent if fill is None else fill)

    def wrap(self, text: str, font, max_w: int, max_lines: int) -> list[str]:
        words, lines, cur = text.split(), [], ""
        for word in words:
            trial = f"{cur} {word}".strip()
            if font.getlength(trial) <= max_w:
                cur = trial
                continue
            if cur:
                lines.append(cur)
            cur = word
            if len(lines) == max_lines:
                break
        if cur and len(lines) < max_lines:
            lines.append(cur)
        if len(lines) == max_lines and " ".join(lines) != " ".join(words):
            lines[-1] = ellipsize(lines[-1] + "…", font, max_w)
        return [ellipsize(l, font, max_w) for l in lines]


def ellipsize(text: str, font, max_w: int) -> str:
    if font.getlength(text) <= max_w:
        return text
    while text and font.getlength(text + "…") > max_w:
        text = text[:-1]
    return text.rstrip() + "…"


def cover(img: Image.Image, size: tuple[int, int]) -> Image.Image:
    """Scale and centre-crop to fill `size` (album art, game headers)."""
    w, h = size
    scale = max(w / img.width, h / img.height)
    img = img.convert("RGB").resize((max(1, round(img.width * scale)), max(1, round(img.height * scale))),
                                    Image.LANCZOS)
    left, top = (img.width - w) // 2, (img.height - h) // 2
    return img.crop((left, top, left + w, top + h))
