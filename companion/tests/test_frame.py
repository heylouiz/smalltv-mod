import numpy as np
import pytest

from smalltv_companion.frame import FULL, PANEL, TILE, Rect, color565, diff, rgb565


def blank():
    return np.zeros((PANEL, PANEL, 3), dtype=np.uint8)


def covered(rects):
    m = np.zeros((PANEL, PANEL), dtype=bool)
    for r in rects:
        assert not m[r.y:r.y2, r.x:r.x2].any(), "rectangles overlap"
        m[r.y:r.y2, r.x:r.x2] = True
    return m


def test_first_frame_is_full():
    assert diff(None, blank()) == [FULL]


def test_identical_frames_send_nothing():
    a = blank()
    assert diff(a, a.copy()) == []


def test_change_below_565_precision_is_ignored():
    a, b = blank(), blank()
    b[10, 10] = (7, 3, 7)            # lost when packed to 5/6/5 bits
    assert diff(a, b) == []


def test_single_pixel_change_is_one_tile():
    a, b = blank(), blank()
    b[100, 37] = (255, 0, 0)
    assert diff(a, b) == [Rect(32, 96, TILE, TILE)]


def test_runs_merge_into_one_rect():
    a, b = blank(), blank()
    b[190:206, 12:228] = 200         # a progress bar
    rects = diff(a, b)
    assert rects == [Rect(0, 176, 240, 32)]


def test_every_changed_pixel_is_covered():
    rng = np.random.default_rng(1)
    a = blank()
    b = a.copy()
    for _ in range(40):
        y, x = rng.integers(0, PANEL, 2)
        b[y, x] = 255
    m = covered(diff(a, b))
    assert m[np.any(a != b, axis=2)].all()


def test_exclude_cuts_the_clock_box_out():
    clock = Rect(0, 0, 240, 64)
    rects = diff(None, blank(), exclude=clock)
    assert rects == [Rect(0, 64, 240, 176)]
    m = covered(diff(None, blank(), exclude=Rect(40, 40, 100, 30)))
    assert m.sum() == PANEL * PANEL - 100 * 30
    assert not m[40:70, 40:140].any()


@pytest.mark.parametrize("o", [Rect(0, 0, 10, 10), Rect(5, 5, 10, 10), Rect(-5, -5, 300, 300), Rect(100, 100, 5, 5)])
def test_subtract_partitions(o):
    r = Rect(0, 0, 20, 20)
    pieces = r.subtract(o)
    total = sum(p.area for p in pieces)
    ix = max(0, min(r.x2, o.x2) - max(r.x, o.x)) * max(0, min(r.y2, o.y2) - max(r.y, o.y))
    assert total == r.area - ix
    covered(pieces)


def test_rgb565_little_endian():
    px = np.array([[[255, 0, 0], [0, 255, 0], [0, 0, 255], [255, 255, 255]]], dtype=np.uint8)
    assert rgb565(px) == bytes([0x00, 0xF8, 0xE0, 0x07, 0x1F, 0x00, 0xFF, 0xFF])
    assert color565((255, 0, 0)) == 0xF800
