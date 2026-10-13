// Stv2.cpp — opening and validating an STV2 file, reading its records, and
// turning slot values into text. Mirrors editor/src/format.js (validation)
// and editor/src/render.js (formatting); the parity tests hold them together.
#include "Stv2.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

namespace stv2 {
namespace {
const uint8_t RecordSize[SecCount] = {0, 8, 8, 20, 4, 6, 8, 12, 8, 8, 12, 36, 16, 16, 12, 0};
const uint16_t MaxRecords[SecCount] = {0, 1024, 16, 512, 1024, 1024, 64, 64, 32, 1024, 4, 64, 64, 8, 2048, 0};

inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline int16_t i16(const uint8_t* p) { return int16_t(u16(p)); }
inline uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

bool fail(char* error, size_t size, const char* msg) {
  if (error && size) snprintf(error, size, "%s", msg);
  return false;
}

// Length of the longest prefix of s[0..len) that ends on a UTF-8 character
// boundary and is at most `cap` bytes.
size_t utf8Cut(const char* s, size_t len, size_t cap) {
  if (len <= cap) return len;
  size_t end = cap;
  while (end > 0 && (uint8_t(s[end]) & 0xC0) == 0x80) end--;
  return end;
}
}  // namespace

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t n) {
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= data[i];
    for (int k = 0; k < 8; k++) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
  }
  return ~crc;
}

bool Theme::record(Sec s, uint16_t i, uint8_t* buf) {
  if (!source_ || i >= count_[s]) return false;
  return source_->read(offset_[s] + uint32_t(i) * RecordSize[s], buf, RecordSize[s]);
}
bool Theme::blob(uint32_t offset, void* dest, size_t n) {
  if (!source_ || offset + n > blobSize_) return false;
  return source_->read(offset_[SecBlob] + offset, dest, n);
}

#define READ(sec, n) uint8_t b[n]; if (!record(sec, i, b)) return false
bool Theme::screen(uint16_t i, Screen& o) { READ(SecScreens, 8); o = {u16(b), u16(b + 2), u16(b + 4), u16(b + 6)}; return true; }
bool Theme::op(uint16_t i, Op& o) {
  READ(SecOps, 20);
  o.type = b[0]; o.flags = b[1]; o.x = i16(b + 2); o.y = i16(b + 4); o.w = i16(b + 6); o.h = i16(b + 8);
  o.color = u16(b + 10); o.color2 = u16(b + 12); o.a = u16(b + 14); o.b = u16(b + 16); o.c = u16(b + 18);
  return true;
}
bool Theme::templ(uint16_t i, Template& o) { READ(SecTemplates, 4); o = {u16(b), u16(b + 2)}; return true; }
bool Theme::part(uint16_t i, Part& o) { READ(SecParts, 6); o = {b[0], u16(b + 2), u16(b + 4)}; return true; }
bool Theme::slot(uint16_t i, Slot& o) { READ(SecSlots, 8); o = {b[0], b[1], u16(b + 2), u16(b + 4), u16(b + 6)}; return true; }
bool Theme::format(uint16_t i, Format& o) { READ(SecFormats, 12); o = {i16(b), b[2], b[3], b[4], u16(b + 6)}; return true; }
bool Theme::table(uint16_t i, Table& o) { READ(SecTables, 8); o = {u16(b), u16(b + 2), u16(b + 4)}; return true; }
bool Theme::entry(uint16_t i, Entry& o) { READ(SecEntries, 8); o = {i16(b), i16(b + 2), u16(b + 4)}; return true; }
bool Theme::sourceRec(uint16_t i, SourceRec& o) { READ(SecSources, 12); o = {u16(b), u16(b + 2), u16(b + 4), u16(b + 6)}; return true; }
bool Theme::field(uint16_t i, Field& o) {
  READ(SecFields, 36);
  o.slot = u16(b); o.depth = b[2];
  for (int k = 0; k < MaxPath; k++) o.path[k] = u32(b + 4 + k * 4);
  return true;
}
bool Theme::image(uint16_t i, Image& o) { READ(SecImages, 16); o = {u16(b), u16(b + 2), u16(b + 4), b[6], b[7], u32(b + 8), u32(b + 12)}; return true; }
bool Theme::font(uint16_t i, Font& o) { READ(SecFonts, 16); o = {u16(b), u16(b + 2), b[4], b[5], b[6], u16(b + 8), u32(b + 12)}; return true; }
bool Theme::glyph(uint16_t i, Glyph& o) { READ(SecGlyphs, 12); o = {u16(b), b[2], b[3], int8_t(b[4]), int8_t(b[5]), b[6], u32(b + 8)}; return true; }
bool Theme::stringRec(uint16_t i, StringRec& o) { READ(SecStrings, 8); o = {u32(b), u16(b + 4)}; return true; }
#undef READ

size_t Theme::string(uint16_t i, char* out, size_t cap) {
  StringRec s;
  if (!cap) return 0;
  out[0] = 0;
  if (!stringRec(i, s)) return 0;
  // Read one byte past the cut when the string is longer, so utf8Cut can see
  // whether the cut lands inside a character.
  size_t n = s.length < cap ? s.length : cap;
  if (!blob(s.offset, out, n)) { out[0] = 0; return 0; }
  n = utf8Cut(out, n, cap - 1);
  out[n] = 0;
  return n;
}

uint16_t Theme::lookup(uint16_t t, long n) {
  Table tab;
  if (!table(t, tab)) return None;
  for (uint16_t i = tab.firstEntry; i < tab.firstEntry + tab.count; i++) {
    Entry e;
    if (!entry(i, e)) return None;
    if (e.from <= n && n <= e.to) return e.value;
  }
  return tab.fallback;
}

// ---- open + validate -----------------------------------------------------------
bool Theme::open(Source& source, char* error, size_t es) {
  source_ = nullptr;
  for (int s = 0; s < SecCount; s++) { offset_[s] = 0; count_[s] = 0; }
  blobSize_ = 0;
  uint8_t h[32];
  uint32_t size = source.size();
  if (size < 32 || !source.read(0, h, 32)) return fail(error, es, "file too short");
  if (memcmp(h, "STV2", 4) != 0) return fail(error, es, "not an STV2 file");
  if (u16(h + 4) != FormatVersion) return fail(error, es, "unsupported format version");
  uint16_t sections = u16(h + 6);
  if (u32(h + 8) != size) return fail(error, es, "file size does not match header");
  if (size > MaxFile) return fail(error, es, "file too large");
  if (u16(h + 16) != Width || u16(h + 18) != Height) return fail(error, es, "unsupported canvas size");
  // CRC over everything after the CRC field, in small chunks.
  uint32_t crc = 0;
  uint8_t chunk[128];
  for (uint32_t at = 16; at < size;) {
    size_t n = size - at < sizeof(chunk) ? size - at : sizeof(chunk);
    if (!source.read(at, chunk, n)) return fail(error, es, "read error");
    crc = crc32Update(crc, chunk, n);
    at += n;
  }
  if (crc != u32(h + 12)) return fail(error, es, "checksum mismatch");
  uint32_t tableEnd = 32 + uint32_t(sections) * 12;
  if (tableEnd > size) return fail(error, es, "section table out of range");
  bool seen[SecCount] = {};
  for (uint16_t i = 0; i < sections; i++) {
    uint8_t e[12];
    if (!source.read(32 + i * 12, e, 12)) return fail(error, es, "read error");
    uint16_t type = u16(e), rs = u16(e + 2);
    uint32_t off = u32(e + 4), len = u32(e + 8);
    if (type == 0 || type >= SecCount) return fail(error, es, "unknown section");
    if (seen[type]) return fail(error, es, "duplicate section");
    seen[type] = true;
    if (rs != RecordSize[type]) return fail(error, es, "bad record size");
    if (off % 4 || off < tableEnd || off + len > size || off + len < off) return fail(error, es, "section out of range");
    offset_[type] = off;
    if (type == SecBlob) { blobSize_ = len; continue; }
    if (len % rs) return fail(error, es, "partial record");
    if (len / rs > MaxRecords[type]) return fail(error, es, "too many records");
    count_[type] = uint16_t(len / rs);
  }
  name_ = u16(h + 20);
  source_ = &source;
  if (!validate(error, es)) { source_ = nullptr; return false; }
  return true;
}

bool Theme::validateImage(const Image& im, char* error, size_t es) {
  if (!im.w || !im.h || im.w > Width || im.h > Height) return fail(error, es, "image: bad size");
  if (!im.frames || im.frames > 256) return fail(error, es, "image: bad frame count");
  if (im.offset + im.size > blobSize_ || im.offset + im.size < im.offset) return fail(error, es, "image: data out of range");
  uint32_t rows = uint32_t(im.h) * im.frames, px = uint32_t(im.w) * rows;
  if (im.encoding == 0) {
    if (im.size != px * 2 + ((im.flags & 1) ? px : 0)) return fail(error, es, "image: size does not match pixels");
    return true;
  }
  if (im.encoding != 1) return fail(error, es, "image: unknown encoding");
  uint8_t b[4];
  if (im.size < 4 || !blob(im.offset, b, 4)) return fail(error, es, "image: truncated");
  uint16_t n = u16(b);
  if (!n || n > 256) return fail(error, es, "image: bad palette");
  uint32_t table = 4 + uint32_t(n) * 3;
  if (table + rows * 4 > im.size) return fail(error, es, "image: truncated");
  // Decode every row once: each must produce exactly w indices < n without
  // leaving the image. Bytes come through a small window to keep RAM flat.
  uint8_t win[64];
  for (uint32_t r = 0; r < rows; r++) {
    if (!blob(im.offset + table + r * 4, b, 4)) return fail(error, es, "read error");
    uint32_t at = u32(b), out = 0, winStart = 0, winLen = 0;
    auto byteAt = [&](uint32_t pos, uint8_t& v) -> bool {
      if (pos >= im.size) return false;
      if (pos < winStart || pos >= winStart + winLen) {
        winStart = pos;
        winLen = im.size - pos < sizeof(win) ? im.size - pos : sizeof(win);
        if (!blob(im.offset + pos, win, winLen)) return false;
      }
      v = win[pos - winStart];
      return true;
    };
    while (out < im.w) {
      uint8_t c, v;
      if (!byteAt(at++, c)) return fail(error, es, "image: row out of range");
      uint32_t run = c < 128 ? c + 1u : c - 126u;
      if (out + run > im.w) return fail(error, es, "image: row overruns width");
      uint32_t literal = c < 128 ? run : 1;
      for (uint32_t k = 0; k < literal; k++) {
        if (!byteAt(at++, v)) return fail(error, es, "image: row out of range");
        if (v >= n) return fail(error, es, "image: index outside palette");
      }
      out += run;
    }
  }
  return true;
}

bool Theme::validate(char* error, size_t es) {
  if (name_ != None && name_ >= count_[SecStrings]) return fail(error, es, "name: bad reference");
  for (uint16_t i = 0; i < count_[SecStrings]; i++) {
    StringRec s; stringRec(i, s);
    if (s.offset + s.length > blobSize_) return fail(error, es, "string: data out of range");
  }
  if (!count_[SecScreens]) return fail(error, es, "theme has no screens");
  for (uint16_t i = 0; i < count_[SecScreens]; i++) {
    Screen s; screen(i, s);
    if (uint32_t(s.firstOp) + s.opCount > count_[SecOps]) return fail(error, es, "screen: range out of bounds");
    if (s.opCount > 96) return fail(error, es, "screen: too many ops");
  }
  for (uint16_t i = 0; i < count_[SecTemplates]; i++) {
    Template t; templ(i, t);
    if (uint32_t(t.firstPart) + t.partCount > count_[SecParts]) return fail(error, es, "template: range out of bounds");
  }
  for (uint16_t i = 0; i < count_[SecParts]; i++) {
    Part p; part(i, p);
    if (p.kind == 0) { if (p.a >= count_[SecStrings]) return fail(error, es, "literal: bad reference"); }
    else if (p.kind == 1) {
      if (p.a >= count_[SecSlots]) return fail(error, es, "value: bad reference");
      if (p.b != None && p.b >= count_[SecFormats]) return fail(error, es, "format: bad reference");
    } else return fail(error, es, "part: bad kind");
  }
  for (uint16_t i = 0; i < count_[SecSlots]; i++) {
    Slot s; slot(i, s);
    if (s.kind > SlotData || s.type > TypeString) return fail(error, es, "slot: bad kind");
    if (s.kind == SlotClock && s.arg > ClockAmPm) return fail(error, es, "slot: bad clock field");
    if (s.type == TypeString && (!s.poolSize || uint32_t(s.poolOffset) + s.poolSize > PoolSize)) return fail(error, es, "slot: string pool out of range");
  }
  for (uint16_t i = 0; i < count_[SecFormats]; i++) {
    Format f; format(i, f);
    if (f.table != None && f.table >= count_[SecTables]) return fail(error, es, "format table: bad reference");
  }
  for (uint16_t i = 0; i < count_[SecTables]; i++) {
    Table t; table(i, t);
    if (uint32_t(t.firstEntry) + t.count > count_[SecEntries]) return fail(error, es, "table: range out of bounds");
  }
  for (uint16_t i = 0; i < count_[SecSources]; i++) {
    SourceRec s; sourceRec(i, s);
    if (s.url >= count_[SecStrings]) return fail(error, es, "source url: bad reference");
    if (uint32_t(s.firstField) + s.fieldCount > count_[SecFields]) return fail(error, es, "source: range out of bounds");
    if (!s.seconds) return fail(error, es, "source: interval must be positive");
  }
  for (uint16_t i = 0; i < count_[SecFields]; i++) {
    Field f; field(i, f);
    if (f.slot >= count_[SecSlots]) return fail(error, es, "field slot: bad reference");
    if (!f.depth || f.depth > MaxPath) return fail(error, es, "field: bad path depth");
  }
  for (uint16_t i = 0; i < count_[SecImages]; i++) {
    Image im; image(i, im);
    if (!validateImage(im, error, es)) return false;
  }
  uint32_t glyphs = 0;
  for (uint16_t i = 0; i < count_[SecFonts]; i++) {
    Font f; font(i, f);
    if (uint32_t(f.firstGlyph) + f.glyphCount > count_[SecGlyphs]) return fail(error, es, "font: range out of bounds");
    if (!f.glyphCount || (f.bpp != 1 && f.bpp != 4)) return fail(error, es, "font: bad glyphs or depth");
    if (f.fallback >= f.glyphCount) return fail(error, es, "font: bad fallback");
    long last = -1;
    for (uint16_t g = f.firstGlyph; g < f.firstGlyph + f.glyphCount; g++) {
      Glyph gl; glyph(g, gl);
      if (long(gl.codepoint) <= last) return fail(error, es, "font: glyphs not sorted");
      last = gl.codepoint;
      if (gl.w > 120 || gl.h > 120) return fail(error, es, "glyph too large");
      uint32_t bytes = uint32_t(gl.h) * ((uint32_t(gl.w) * f.bpp + 7) / 8);
      if (f.offset + gl.offset + bytes > blobSize_) return fail(error, es, "glyph: data out of range");
    }
    glyphs += f.glyphCount;
  }
  if (glyphs > 2048) return fail(error, es, "too many glyphs");
  for (uint16_t i = 0; i < count_[SecOps]; i++) {
    Op o; op(i, o);
    switch (o.type) {
      case OpRect: case OpCircle: break;
      case OpImage: {
        if (o.a >= count_[SecImages]) return fail(error, es, "image op: bad reference");
        Image im; image(o.a, im);
        if (o.w != im.w || o.h != im.h) return fail(error, es, "image op: size does not match image");
        if (o.flags & FrameValue) {
          if (o.b >= count_[SecTables]) return fail(error, es, "image frame table: bad reference");
          if (o.c >= count_[SecSlots]) return fail(error, es, "image frame slot: bad reference");
        } else if (!(o.flags & Animate) && o.c >= im.frames) return fail(error, es, "image op: bad frame");
        break;
      }
      case OpText:
        if (o.a >= count_[SecFonts]) return fail(error, es, "text font: bad reference");
        if (o.b >= count_[SecTemplates]) return fail(error, es, "text template: bad reference");
        break;
      default: return fail(error, es, "bad op type");
    }
  }
  return true;
}

// ---- values ----------------------------------------------------------------------
void Values::setString(uint16_t i, const Slot& slot, const char* text, size_t len) {
  if (i >= MaxSlots || slot.type != TypeString) return;
  size_t n = utf8Cut(text, len, slot.poolSize);
  // Stored NUL-terminated when it fits; a full slot is exactly poolSize bytes.
  memcpy(pool + slot.poolOffset, text, n);
  if (n < slot.poolSize) pool[slot.poolOffset + n] = 0;
  number[i] = double(n);   // the string's length
  has |= uint64_t(1) << i;
}

void applyClock(Theme& theme, Values& values, const ClockTime* t, const char* city) {
  uint16_t n = theme.count(SecSlots);
  for (uint16_t i = 0; i < n && i < MaxSlots; i++) {
    Slot s;
    if (!theme.slot(i, s)) continue;
    if (s.kind == SlotCity) {
      if (city && city[0]) values.setString(i, s, city, strlen(city));
      else values.unset(i);
      continue;
    }
    if (s.kind != SlotClock) continue;
    if (!t) { values.unset(i); continue; }
    int v = 0;
    switch (s.arg) {
      case ClockH24: v = t->hour; break;
      case ClockH12: v = t->hour % 12 ? t->hour % 12 : 12; break;
      case ClockMin: v = t->minute; break;
      case ClockSec: v = t->second; break;
      case ClockDay: v = t->day; break;
      case ClockMonth: v = t->month; break;
      case ClockYear: v = t->year; break;
      case ClockWeekday: v = t->weekday; break;
      case ClockAmPm: v = t->hour < 12 ? 0 : 1; break;
    }
    values.setNumber(i, v);
  }
}

namespace {
bool append(char* out, size_t cap, size_t& len, const char* s, size_t n) {
  if (len + n >= cap) n = cap - 1 - len;
  memcpy(out + len, s, n);
  len += n;
  out[len] = 0;
  return true;
}
// Rounds half away from zero at `decimals` places and prints the result like
// JS toFixed of the rounded number: integer digits zero-padded to `width`.
void printNumber(double v, uint8_t decimals, uint8_t width, char* out, size_t cap) {
  double p = 1;
  for (uint8_t i = 0; i < decimals; i++) p *= 10;
  double scaled = floor(fabs(v) * p + 0.5);
  bool negative = v < 0 && scaled > 0;
  unsigned long long whole = (unsigned long long)(scaled / p);
  unsigned long long frac = (unsigned long long)(scaled - double(whole) * p + 0.5);
  if (decimals && frac >= (unsigned long long)p) { whole++; frac -= (unsigned long long)p; }
  char digits[24];
  int nd = snprintf(digits, sizeof(digits), "%llu", whole);
  size_t len = 0;
  if (negative && len + 1 < cap) out[len++] = '-';
  for (int pad = width - nd; pad > 0 && len + 1 < cap; pad--) out[len++] = '0';
  for (int i = 0; i < nd && len + 1 < cap; i++) out[len++] = digits[i];
  if (decimals && len + 1 < cap) {
    out[len++] = '.';
    char f[24];
    snprintf(f, sizeof(f), "%0*llu", int(decimals), frac);
    for (int i = 0; f[i] && len + 1 < cap; i++) out[len++] = f[i];
  }
  out[len] = 0;
}
}  // namespace

bool appendValue(Theme& theme, const Values& values, uint16_t slotIndex, uint16_t fmt, char* out, size_t cap, size_t& len) {
  if (!values.hasValue(slotIndex)) return append(out, cap, len, "--", 2);
  Slot s;
  if (!theme.slot(slotIndex, s)) return false;
  if (s.type == TypeString) {
    const char* p = values.pool + s.poolOffset;
    size_t n = size_t(values.number[slotIndex]);
    return append(out, cap, len, p, n);
  }
  double v = values.number[slotIndex];
  char num[48];
  if (fmt == None) {
    // Plain numbers print like JS String(n): integers without a fraction.
    if (v == floor(v) && fabs(v) < 1e15) printNumber(v, 0, 0, num, sizeof(num));
    else snprintf(num, sizeof(num), "%.10g", v);
    return append(out, cap, len, num, strlen(num));
  }
  Format f;
  if (!theme.format(fmt, f)) return false;
  double p = 1;
  for (uint8_t i = 0; i < f.decimals; i++) p *= 10;
  double x = v + f.add;
  double n = (x < 0 ? -1 : 1) * floor(fabs(x) * p + 0.5) / p;
  if (f.mod) {
    long r = long(floor(n + 0.5)) % f.mod;
    n = double((r + f.mod) % f.mod);
  }
  if (f.table != None) {
    uint16_t hit = theme.lookup(f.table, long(floor(n + 0.5)));
    if (hit != None) {
      char str[MaxText + 1];
      size_t sl = theme.string(hit, str, sizeof(str));
      return append(out, cap, len, str, sl);
    }
  }
  printNumber(n, f.decimals, f.width, num, sizeof(num));
  return append(out, cap, len, num, strlen(num));
}

bool resolveText(Theme& theme, const Values& values, uint16_t ti, char* out, size_t cap) {
  Template t;
  size_t len = 0;
  out[0] = 0;
  if (!theme.templ(ti, t)) return false;
  for (uint16_t i = t.firstPart; i < t.firstPart + t.partCount && len + 1 < cap; i++) {
    Part p;
    if (!theme.part(i, p)) return false;
    if (p.kind == 0) {
      char lit[MaxText + 1];
      size_t n = theme.string(p.a, lit, sizeof(lit));
      append(out, cap, len, lit, n);
    } else if (!appendValue(theme, values, p.a, p.b, out, cap, len)) return false;
  }
  len = utf8Cut(out, len, MaxText);
  out[len] = 0;
  return true;
}

bool expandUrl(const char* url, double lat, double lon, char* out, size_t cap) {
  bool needs = strstr(url, "{lat}") || strstr(url, "{lon}");
  if (needs && lat == 0 && lon == 0) return false;
  size_t len = 0;
  for (const char* p = url; *p;) {
    const char* tok = nullptr;
    double v = 0;
    if (!strncmp(p, "{lat}", 5)) { tok = p; v = lat; }
    else if (!strncmp(p, "{lon}", 5)) { tok = p; v = lon; }
    if (tok) {
      char num[24];
      printNumber(v, 4, 0, num, sizeof(num));
      size_t n = strlen(num);
      if (len + n >= cap) return false;
      memcpy(out + len, num, n);
      len += n;
      p += 5;
    } else {
      if (len + 1 >= cap) return false;
      out[len++] = *p++;
    }
  }
  out[len] = 0;
  return true;
}

}  // namespace stv2
