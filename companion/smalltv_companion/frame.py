"""Pixel plumbing: RGB565 packing and the frame diff that keeps pushes small.

The device takes rectangles of little-endian RGB565 and draws them where it
is told, so the cheapest update is the smallest set of rectangles covering
what changed. Frames are compared in TILE x TILE blocks; changed tiles are
merged into horizontal runs per tile row, and runs with the same span in
consecutive rows into taller rectangles.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np
from PIL import Image

PANEL = 240
TILE = 16


@dataclass(frozen=True)
class Rect:
    x: int
    y: int
    w: int
    h: int

    @property
    def x2(self) -> int:
        return self.x + self.w

    @property
    def y2(self) -> int:
        return self.y + self.h

    @property
    def area(self) -> int:
        return self.w * self.h

    def intersects(self, o: "Rect") -> bool:
        return self.x < o.x2 and o.x < self.x2 and self.y < o.y2 and o.y < self.y2

    def subtract(self, o: "Rect") -> list["Rect"]:
        """This rectangle minus `o`, as up to four disjoint rectangles."""
        if not self.intersects(o):
            return [self]
        out = []
        if o.y > self.y:                                   # band above
            out.append(Rect(self.x, self.y, self.w, o.y - self.y))
        if o.y2 < self.y2:                                 # band below
            out.append(Rect(self.x, o.y2, self.w, self.y2 - o.y2))
        top, bottom = max(self.y, o.y), min(self.y2, o.y2)
        if o.x > self.x:                                   # left of o, middle band
            out.append(Rect(self.x, top, o.x - self.x, bottom - top))
        if o.x2 < self.x2:                                 # right of o, middle band
            out.append(Rect(o.x2, top, self.x2 - o.x2, bottom - top))
        return out


FULL = Rect(0, 0, PANEL, PANEL)


def to_array(img: Image.Image) -> np.ndarray:
    return np.asarray(img.convert("RGB"), dtype=np.uint8)


def rgb565(arr: np.ndarray) -> bytes:
    """H x W x 3 uint8 -> little-endian RGB565 bytes, row-major."""
    a = arr.astype(np.uint16)
    v = ((a[..., 0] & 0xF8) << 8) | ((a[..., 1] & 0xFC) << 3) | (a[..., 2] >> 3)
    return v.astype("<u2").tobytes()


def color565(rgb: tuple[int, int, int]) -> int:
    r, g, b = rgb[:3]
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def diff(prev: np.ndarray | None, cur: np.ndarray, exclude: Rect | None = None) -> list[Rect]:
    """Rectangles covering every pixel that differs between two frames.

    With no previous frame everything is new. `exclude` (the device-drawn
    clock box) is cut out of the result, so a push never paints over it.
    """
    if prev is None:
        rects = [FULL]
    else:
        # Compare in the 565 space the panel actually shows: a change below
        # its precision would cost a push and change nothing on screen.
        changed = np.any((prev ^ cur) & np.array([0xF8, 0xFC, 0xF8], dtype=np.uint8), axis=2)
        th, tw = PANEL // TILE, PANEL // TILE
        tiles = changed.reshape(th, TILE, tw, TILE).any(axis=(1, 3))
        rects = _merge(tiles)
    if exclude is not None:
        rects = [piece for r in rects for piece in r.subtract(exclude)]
    return [r for r in rects if r.area > 0]


def _merge(tiles: np.ndarray) -> list[Rect]:
    open_runs: dict[tuple[int, int], Rect] = {}
    done: list[Rect] = []
    for ty in range(tiles.shape[0]):
        runs = []
        tx = 0
        while tx < tiles.shape[1]:
            if tiles[ty, tx]:
                start = tx
                while tx < tiles.shape[1] and tiles[ty, tx]:
                    tx += 1
                runs.append((start, tx))
            tx += 1
        nxt: dict[tuple[int, int], Rect] = {}
        for span in runs:
            r = open_runs.pop(span, None)
            if r is not None:
                nxt[span] = Rect(r.x, r.y, r.w, r.h + TILE)
            else:
                nxt[span] = Rect(span[0] * TILE, ty * TILE, (span[1] - span[0]) * TILE, TILE)
        done.extend(open_runs.values())
        open_runs = nxt
    done.extend(open_runs.values())
    return sorted(done, key=lambda r: (r.y, r.x))
