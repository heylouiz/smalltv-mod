---
title: Theme v2 format (STV2)
description: The compiled binary theme format the browser editor produces and the device renders straight from flash.
---

A theme v2 is authored in the browser editor and compiled there into one
binary file, `STV2`. The device never parses text: it validates the file once,
then reads records and pixels from flash as it draws, so its RAM use does not
grow with the theme. This page is the contract between the compiler
(`editor/src/`) and the device renderer (`src/features/theme2/`); both must
follow it exactly, and the parity tests compare their output pixel by pixel.

## Layout

All integers are little-endian. The file starts with a 32-byte header:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | `char[4]` | Magic `STV2` |
| 4 | `u16` | Format version, `1` |
| 6 | `u16` | Section count |
| 8 | `u32` | File size in bytes |
| 12 | `u32` | CRC-32 (IEEE) of bytes 16 to the end |
| 16 | `u16` | Width, `240` |
| 18 | `u16` | Height, `240` |
| 20 | `u16` | Theme name (string index) |
| 22 | `u16` | Flags, `0` |
| 24 | `u8[8]` | Zero |

A section table follows at offset 32, one 12-byte entry per section:
`u16 type`, `u16 recordSize`, `u32 offset`, `u32 size`. Each type appears at
most once. Sections start on a 4-byte boundary and lie inside the file;
`size` is a multiple of `recordSize`, which must equal the size below (the BLOB
has record size `0`). A missing section has no records.

| Type | Section | Record | Fields |
| --- | --- | --- | --- |
| 1 | STRINGS | 8 | `u32 offset` (BLOB), `u16 length`, `u16 0`. UTF-8 |
| 2 | SCREENS | 8 | `u16 firstOp`, `u16 opCount`, `u16 seconds`, `u16 background` |
| 3 | OPS | 20 | `u8 type`, `u8 flags`, `i16 x`, `i16 y`, `i16 w`, `i16 h`, `u16 color`, `u16 color2`, `u16 a`, `u16 b`, `u16 c` |
| 4 | TEMPLATES | 4 | `u16 firstPart`, `u16 partCount` |
| 5 | PARTS | 6 | `u8 kind`, `u8 0`, `u16 a`, `u16 b` |
| 6 | SLOTS | 8 | `u8 kind`, `u8 type`, `u16 arg`, `u16 poolOffset`, `u16 poolSize` |
| 7 | FORMATS | 12 | `i16 add`, `u8 decimals`, `u8 width`, `u8 mod`, `u8 0`, `u16 table`, `u32 0` |
| 8 | TABLES | 8 | `u16 firstEntry`, `u16 count`, `u16 fallback`, `u16 0` |
| 9 | ENTRIES | 8 | `i16 from`, `i16 to`, `u16 value`, `u16 0` |
| 10 | SOURCES | 12 | `u16 url` (string), `u16 seconds`, `u16 firstField`, `u16 fieldCount`, `u32 0` |
| 11 | FIELDS | 36 | `u16 slot`, `u8 depth`, `u8 0`, `u32 path[8]` |
| 12 | IMAGES | 16 | `u16 w`, `u16 h`, `u16 frames`, `u8 encoding`, `u8 flags`, `u32 offset` (BLOB), `u32 size` |
| 13 | FONTS | 16 | `u16 firstGlyph`, `u16 glyphCount`, `u8 bpp`, `u8 lineHeight`, `u8 ascent`, `u8 0`, `u16 fallback`, `u16 0`, `u32 offset` (BLOB) |
| 14 | GLYPHS | 12 | `u16 codepoint`, `u8 w`, `u8 h`, `i8 x`, `i8 y`, `u8 advance`, `u8 0`, `u32 offset` |
| 15 | BLOB | 0 | Raw bytes referenced by the sections above |

Colors are RGB565. `0xFFFF` in an index field means "none".

## Screens and ops

Screens rotate in order, each shown for `seconds` (`0` keeps the screen until
the theme changes; a single-screen theme uses `0`). A screen draws its
background, then its ops `firstOp .. firstOp + opCount - 1` in order, each
painted over the previous ones.

| Type | Op | Meaning |
| --- | --- | --- |
| 1 | Rect | Fills `x, y, w, h` with `color`; `a` is the corner radius |
| 2 | Circle | Fills the circle inscribed in the `w`×`w` box at `x, y` |
| 3 | Image | Draws image `a` at `x, y`; `w`, `h` repeat its frame size |
| 4 | Text | Draws template `b` with font `a` in `color`, clipped to the box |

An image's frame is `c`, unless a flag selects another source:

- flag 1: the frame is table `b` looked up with the value of slot `c`
  rounded to an integer (weather icons). Without a number in the slot, or with
  no matching entry, the frame is the table's `fallback`, or 0 if it has none;
- flag 2: the frame is `floor(t · c / 1000)`, where `t` is the milliseconds
  since the theme started, so it advances at `c` frames per second.

The frame is then taken modulo the image's frame count.

Text flags 0–1 set the alignment inside the box: `0` left, `1` center,
`2` right. The first line's baseline sits `ascent` pixels below `y`. Text never
wraps and never leaves its box.

Pixel tests use doubled coordinates so both renderers agree without floating
point. A pixel `(px, py)` belongs to a circle of box `x, y, w` when
`(2px + 1 − 2x − w)² + (2py + 1 − 2y − w)² ≤ w²`. A rounded rect contains a
pixel unless the pixel lies in a corner square of side `r` and outside the
circle of radius `r` centered at that corner's inner point, tested the same
way.

Alpha blends each channel in its own 5- or 6-bit range:
`out = (src·a + dst·(255 − a) + 127) / 255`, integer division.

## Values and templates

A template is a list of parts: kind `0` writes literal string `a`; kind `1`
writes the value of slot `a` through format `b`.

Slots hold the values a theme can show. Kind `0` is the clock, with `arg`
choosing the field: `0` hour (0–23), `1` hour (1–12), `2` minute, `3` second,
`4` day, `5` month (1–12), `6` year, `7` weekday (0 = Sunday), `8` AM/PM
(0 or 1). Kind `1` is the city from the device's Location settings. Kind `2`
is a data field filled by a source. Type `0` holds a number, type `1` a string
stored in the device's 1,024-byte string pool at `poolOffset`, at most
`poolSize` bytes. A slot without a value yet (no clock sync, no data) renders
as `--`.

A format turns a number into text: add `add`; round to `decimals` places
(halves away from zero); if `mod` is not zero, wrap the integer into
`0 .. mod − 1`; then, if `table` is set, write the table's string for that
integer, otherwise write the number zero-padded to `width` integer digits.
A table maps the first entry with `from ≤ value ≤ to` to its `value` (a string
index for text, a frame number for images), and to `fallback` when none
matches. A table format with no match and no fallback writes the number
instead. Strings pass through formats unchanged. A template's text is cut at
96 bytes, on a character boundary.

## Data sources

A source is an `http://` URL fetched every `seconds`. `{lat}` and `{lon}` in
the URL are replaced with the device location, four decimals. The response is
JSON, read as a stream: each scalar's path is compared with the source's
fields, and a match writes the value into the field's slot: a number (or a
boolean, as 1 or 0) into a number slot, a string into a string slot (cut at
`poolSize` bytes, on a character boundary). Other values and mismatched types
are ignored, and a later match overwrites an earlier one. Nothing else of the
response is kept, so its size is not limited. A source whose URL needs the
location is skipped until one is set.

A path is up to 8 tokens. An object key is its FNV-1a 32-bit hash with the top
bit cleared; an array index `i` is `0x80000000 | i`. `daily.temperature_2m_max[0]`
is three tokens: hash of `daily`, hash of `temperature_2m_max`, index 0.

## Images

Encoding `0` is raw RGB565, `w × h × frames` pixels, followed by as many alpha
bytes when flag 1 is set. Encoding `1` is palette-RLE:

1. `u16` palette size (1–256), `u16 0`
2. the palette colors, `u16` each, then one alpha byte per color
3. one `u32` per row (all frames stacked), the row's offset from the image's
   start
4. the rows, each decoding to exactly `w` palette indices: a control byte `c`
   below 128 copies the next `c + 1` bytes; from 128 up it repeats the next
   byte `c − 126` times

## Fonts

A font's glyphs are sorted by codepoint, which may be any Unicode BMP
character, so accents work. A glyph's bitmap is `h` rows of `ceil(w·bpp/8)`
bytes at `offset` from the font's BLOB offset, most significant bit first. At
4 bpp a pixel's coverage `v` is alpha `v · 17`; at 1 bpp it is 0 or 255. `x`
and `y` place the bitmap relative to the pen on the baseline; the pen then
moves `advance` pixels. A codepoint the font lacks draws glyph `fallback`.

## Limits

| Item | Limit |
| --- | --- |
| File | 900 KiB |
| Screens | 16 |
| Ops | 512, 96 per screen |
| Slots | 64; string pool 1,024 bytes |
| Sources | 4, with 64 fields in all |
| Images | 64, up to 240×240 per frame, 256 frames |
| Fonts | 8, with 2,048 glyphs in all, up to 120×120 each |
| Text | 96 bytes per op after formatting |
| Strings, parts, templates | 1,024 each |
| Tables, formats | 32 and 64, with 1,024 entries in all |
