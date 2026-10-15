"""The clock's digit atlas ("SCG1", see src/features/companion/CompanionProto.h).

The device has no font engine worth the name, so the companion renders
"0123456789:" in whatever font the config names, anti-aliased over the clock
background, and uploads the pixels once. The device then composes HH:MM
every minute by copying glyphs; the PC sends nothing while the time ticks.
"""
from __future__ import annotations

import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFont

from .frame import rgb565

CHARS = "0123456789:"


def render_glyphs(font: ImageFont.FreeTypeFont, fg, bg) -> list[Image.Image]:
    # One shared vertical extent (the union of every glyph's ink) keeps the
    # baseline identical across glyphs and trims the font's empty leading.
    tops, bottoms = [], []
    for ch in CHARS:
        l, t, r, b = font.getbbox(ch, anchor="ls")
        tops.append(t)
        bottoms.append(b)
    top, bottom = min(tops), max(bottoms)
    height = bottom - top
    # Tabular digits: every digit as wide as the widest, so the clock does
    # not jitter sideways as the minutes change.
    digit_w = max(round(font.getlength(c)) for c in CHARS[:10])
    images = []
    for ch in CHARS:
        w = digit_w if ch != ":" else max(1, round(font.getlength(ch)))
        img = Image.new("RGB", (w, height), bg)
        ImageDraw.Draw(img).text((w / 2, -top), ch, font=font, fill=fg, anchor="ms")
        images.append(img)
    return images


def pack_atlas(images: list[Image.Image]) -> bytes:
    if len(images) != len(CHARS):
        raise ValueError("atlas needs exactly 11 glyphs")
    height = images[0].height
    if not 0 < height <= 240 or any(i.height != height for i in images):
        raise ValueError("glyphs must share one height of 1..240 px")
    if any(not 0 < i.width <= 240 for i in images):
        raise ValueError("glyph widths must be 1..240 px")
    out = bytearray(b"SCG1" + struct.pack("<BBH", len(CHARS), height, 0))
    out += bytes(i.width for i in images)
    for img in images:
        out += rgb565(np.asarray(img.convert("RGB")))
    return bytes(out)


def atlas_width(images: list[Image.Image], text: str = "00:00") -> int:
    return sum(images[CHARS.index(c)].width for c in text)


def preview(images: list[Image.Image], text: str) -> Image.Image:
    """The clock as the device will compose it (used for local previews)."""
    w = atlas_width(images, text)
    out = Image.new("RGB", (w, images[0].height))
    x = 0
    for c in text:
        g = images[CHARS.index(c)]
        out.paste(g, (x, 0))
        x += g.width
    return out
