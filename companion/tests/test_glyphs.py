import struct

from smalltv_companion import glyphs
from smalltv_companion.render import load_font


def test_atlas_layout_matches_firmware_header():
    images = glyphs.render_glyphs(load_font("DejaVu Sans:bold", 40), (255, 255, 255), (0, 0, 0))
    data = glyphs.pack_atlas(images)
    assert data[:4] == b"SCG1"
    count, height, _ = struct.unpack_from("<BBH", data, 4)
    assert count == 11 and height == images[0].height
    widths = list(data[8:19])
    assert widths == [i.width for i in images]
    assert len(set(widths[:10])) == 1           # tabular digits
    assert len(data) == 19 + sum(w * height * 2 for w in widths)


def test_digits_fit_a_default_clock_box():
    images = glyphs.render_glyphs(load_font("DejaVu Sans:bold", 52), (255, 255, 255), (0, 0, 0))
    assert glyphs.atlas_width(images) <= 240
    assert images[0].height <= 64
    assert len(glyphs.pack_atlas(images)) < 64 * 1024   # device cap
