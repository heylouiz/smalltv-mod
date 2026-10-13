// Stv2Render.cpp — draws one horizontal band of a screen. Every rule here
// (integer geometry, blending, text layout, frame choice) mirrors
// editor/src/render.js; tests/theme2 compares the two pixel by pixel.
#include "Stv2.h"
#include <math.h>
#include <string.h>

namespace stv2 {
namespace {

inline uint16_t blend(uint16_t src, uint16_t dst, uint8_t a) {
  if (a == 255) return src;
  if (a == 0) return dst;
  auto mix = [a](unsigned s, unsigned d) { return (s * a + d * (255u - a) + 127u) / 255u; };
  unsigned r = mix(src >> 11, dst >> 11), g = mix((src >> 5) & 63, (dst >> 5) & 63), b = mix(src & 31, dst & 31);
  return uint16_t((r << 11) | (g << 5) | b);
}
inline int floorDiv(int a, int b) { return (a >= 0) ? a / b : -((-a + b - 1) / b); }
inline bool inCircle(int px, int py, int cx2, int cy2, int d) {
  long dx = 2L * px + 1 - cx2, dy = 2L * py + 1 - cy2;
  return dx * dx + dy * dy <= long(d) * d;
}

struct Band {
  uint16_t* px;
  int y0, rows;
  int top() const { return y0 < 0 ? 0 : y0; }
  int bottom() const { return y0 + rows > Height ? Height : y0 + rows; }
  uint16_t& at(int x, int y) { return px[(y - y0) * Width + x]; }
};

void drawRect(Band& band, const Op& op) {
  int r = op.a;
  if (r > (op.w >> 1)) r = op.w >> 1;
  if (r > (op.h >> 1)) r = op.h >> 1;
  int ya = op.y > band.top() ? op.y : band.top(), yb = op.y + op.h < band.bottom() ? op.y + op.h : band.bottom();
  int xa = op.x > 0 ? op.x : 0, xb = op.x + op.w < Width ? op.x + op.w : Width;
  for (int y = ya; y < yb; y++) {
    for (int x = xa; x < xb; x++) {
      if (r) {
        bool left = x < op.x + r, right = x >= op.x + op.w - r, top = y < op.y + r, bottom = y >= op.y + op.h - r;
        if ((left || right) && (top || bottom)) {
          int cx2 = 2 * (left ? op.x + r : op.x + op.w - r), cy2 = 2 * (top ? op.y + r : op.y + op.h - r);
          if (!inCircle(x, y, cx2, cy2, 2 * r)) continue;
        }
      }
      band.at(x, y) = op.color;
    }
  }
}

void drawCircle(Band& band, const Op& op) {
  int ya = op.y > band.top() ? op.y : band.top(), yb = op.y + op.w < band.bottom() ? op.y + op.w : band.bottom();
  int xa = op.x > 0 ? op.x : 0, xb = op.x + op.w < Width ? op.x + op.w : Width;
  for (int y = ya; y < yb; y++)
    for (int x = xa; x < xb; x++)
      if (inCircle(x, y, 2 * op.x + op.w, 2 * op.y + op.w, op.w)) band.at(x, y) = op.color;
}

// Little-endian u16s read as bytes into the same array, converted in place.
void fixU16(uint16_t* words, size_t n) {
  uint8_t* b = reinterpret_cast<uint8_t*>(words);
  for (size_t i = 0; i < n; i++) words[i] = uint16_t(b[2 * i] | (b[2 * i + 1] << 8));
}

bool imageRow(Theme& theme, uint16_t index, const Image& im, uint16_t frame, int row, Scratch& s) {
  uint32_t r = uint32_t(frame) * im.h + row;
  if (im.encoding == 0) {
    if (!theme.blob(im.offset + r * im.w * 2, s.colors, im.w * 2u)) return false;
    fixU16(s.colors, im.w);
    if (im.flags & 1) {
      uint32_t px = uint32_t(im.w) * im.h * im.frames;
      return theme.blob(im.offset + px * 2 + r * im.w, s.alpha, im.w);
    }
    memset(s.alpha, 255, im.w);
    return true;
  }
  uint8_t b[4];
  if (!theme.blob(im.offset, b, 2)) return false;
  uint16_t n = uint16_t(b[0] | (b[1] << 8));
  if (s.paletteImage != index) {
    if (!theme.blob(im.offset + 4, s.palette, n * 2u) || !theme.blob(im.offset + 4 + n * 2u, s.paletteAlpha, n)) return false;
    fixU16(s.palette, n);
    s.paletteImage = index;
  }
  if (!theme.blob(im.offset + 4 + n * 3u + r * 4, b, 4)) return false;
  uint32_t at = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
  uint32_t avail = im.size - at;
  uint32_t want = avail < sizeof(s.rle) ? avail : sizeof(s.rle);
  if (!theme.blob(im.offset + at, s.rle, want)) return false;
  uint32_t i = 0;
  int out = 0;
  while (out < im.w && i < want) {
    uint8_t c = s.rle[i++];
    if (c < 128) {
      for (int k = 0; k <= c && out < im.w && i < want; k++, out++) {
        uint8_t v = s.rle[i++];
        s.colors[out] = s.palette[v];
        s.alpha[out] = s.paletteAlpha[v];
      }
    } else {
      uint8_t v = s.rle[i++];
      for (int k = 0; k < c - 126 && out < im.w; k++, out++) {
        s.colors[out] = s.palette[v];
        s.alpha[out] = s.paletteAlpha[v];
      }
    }
  }
  return out == im.w;
}

bool drawImage(Theme& theme, const Values& values, Band& band, const Op& op, uint32_t timeMs, Scratch& s) {
  Image im;
  if (!theme.image(op.a, im)) return false;
  uint16_t frame = imageFrame(theme, values, op, timeMs);
  int ya = op.y > band.top() ? op.y : band.top(), yb = op.y + im.h < band.bottom() ? op.y + im.h : band.bottom();
  for (int y = ya; y < yb; y++) {
    if (!imageRow(theme, op.a, im, frame, y - op.y, s)) return false;
    for (int x = 0; x < im.w; x++) {
      int px = op.x + x;
      if (px < 0 || px >= Width) continue;
      band.at(px, y) = blend(s.colors[x], band.at(px, y), s.alpha[x]);
    }
  }
  return true;
}

// Next codepoint of a UTF-8 string; malformed bytes become U+FFFD.
uint32_t nextCodepoint(const char*& p) {
  uint8_t c = uint8_t(*p++);
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (extra < 0) return 0xFFFD;
  uint32_t cp = c & (0x3F >> extra);
  for (int i = 0; i < extra; i++) {
    if ((uint8_t(*p) & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (uint8_t(*p++) & 0x3F);
  }
  return cp;
}

bool drawText(Theme& theme, const Values& values, Band& band, const Op& op, Scratch& s) {
  Font font;
  if (!theme.font(op.a, font)) return false;
  if (!resolveText(theme, values, op.b, s.text, sizeof(s.text))) return false;
  int width = 0;
  Glyph g;
  for (const char* p = s.text; *p;) {
    if (!findGlyph(theme, s, op.a, font, nextCodepoint(p), g)) return false;
    width += g.advance;
  }
  int align = op.flags & 3;
  int pen = op.x + (align == 1 ? floorDiv(op.w - width, 2) : align == 2 ? op.w - width : 0);
  int baseline = op.y + font.ascent;
  int x0 = op.x > 0 ? op.x : 0, x1 = op.x + op.w < Width ? op.x + op.w : Width;
  int y0 = op.y > band.top() ? op.y : band.top(), y1 = op.y + op.h < band.bottom() ? op.y + op.h : band.bottom();
  uint8_t row[60];
  uint8_t mask = uint8_t((1 << font.bpp) - 1);
  for (const char* p = s.text; *p;) {
    if (!findGlyph(theme, s, op.a, font, nextCodepoint(p), g)) return false;
    int stride = (g.w * font.bpp + 7) / 8;
    for (int gy = 0; gy < g.h; gy++) {
      int y = baseline + g.y + gy;
      if (y < y0 || y >= y1) continue;
      if (!theme.blob(font.offset + g.offset + uint32_t(gy) * stride, row, stride)) return false;
      for (int gx = 0; gx < g.w; gx++) {
        int x = pen + g.x + gx;
        if (x < x0 || x >= x1) continue;
        int bit = gx * font.bpp;
        uint8_t v = uint8_t((row[bit >> 3] >> (8 - font.bpp - (bit & 7))) & mask);
        uint8_t a = font.bpp == 4 ? uint8_t(v * 17) : (v ? 255 : 0);
        if (a) band.at(x, y) = blend(op.color, band.at(x, y), a);
      }
    }
    pen += g.advance;
  }
  return true;
}

}  // namespace

uint16_t imageFrame(Theme& theme, const Values& values, const Op& op, uint32_t timeMs) {
  Image im;
  if (!theme.image(op.a, im) || !im.frames) return 0;
  uint32_t frame = op.c;
  if (op.flags & FrameValue) {
    Slot slot;
    bool number = values.hasValue(op.c) && theme.slot(op.c, slot) && slot.type == TypeNumber;
    if (number) frame = theme.lookup(op.b, long(floor(values.number[op.c] + 0.5)));
    else {
      Table t;
      frame = theme.table(op.b, t) ? t.fallback : None;
    }
    if (frame == None) frame = 0;
  } else if (op.flags & Animate) {
    frame = uint32_t((uint64_t(timeMs) * op.c) / 1000);
  }
  return uint16_t(frame % im.frames);
}

bool findGlyph(Theme& theme, Scratch& s, uint16_t fontIndex, const Font& font, uint32_t cp, Glyph& out) {
  unsigned slot = (cp ^ (fontIndex * 31u)) & 15u;
  if ((s.glyphValid[slot >> 3] >> (slot & 7)) & 1) {
    const Scratch::CachedGlyph& c = s.glyphs[slot];
    if (c.font == fontIndex && c.codepoint == cp && cp <= 0xFFFF) { out = c.glyph; return true; }
  }
  int lo = font.firstGlyph, hi = font.firstGlyph + font.glyphCount - 1;
  bool found = false;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (!theme.glyph(uint16_t(mid), out)) return false;
    if (out.codepoint == cp) { found = true; break; }
    if (out.codepoint < cp) lo = mid + 1; else hi = mid - 1;
  }
  if (!found && !theme.glyph(uint16_t(font.firstGlyph + font.fallback), out)) return false;
  if (cp <= 0xFFFF) {
    s.glyphs[slot] = {fontIndex, uint16_t(cp), out};
    s.glyphValid[slot >> 3] |= uint8_t(1 << (slot & 7));
  }
  return true;
}

bool renderBand(Theme& theme, const Values& values, uint16_t screenIndex, uint32_t timeMs,
                int y0, int rows, uint16_t* px, Scratch& s) {
  Screen screen;
  if (!theme.screen(screenIndex, screen)) return false;
  for (int i = 0; i < Width * rows; i++) px[i] = screen.background;
  Band band{px, y0, rows};
  for (uint16_t i = screen.firstOp; i < screen.firstOp + screen.opCount; i++) {
    Op op;
    if (!theme.op(i, op)) return false;
    // Skip ops whose box misses the band; text boxes and images are bounded
    // by their own height, circles by their diameter.
    int h = op.type == OpCircle ? op.w : op.h;
    if (op.y >= y0 + rows || op.y + h <= y0) continue;
    bool ok = true;
    switch (op.type) {
      case OpRect: drawRect(band, op); break;
      case OpCircle: drawCircle(band, op); break;
      case OpImage: ok = drawImage(theme, values, band, op, timeMs, s); break;
      case OpText: ok = drawText(theme, values, band, op, s); break;
    }
    if (!ok) return false;
  }
  return true;
}

}  // namespace stv2
