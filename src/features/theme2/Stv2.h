// Stv2.h — theme v2 core: reads, validates and renders STV2 files.
//
// Portable C++11 with no Arduino dependency, so tests/theme2 builds it on the
// host and checks it pixel for pixel against the browser's reference renderer
// (editor/src/render.js). docs/.../theme2-format.md is the spec.
//
// Memory model: the theme stays in flash. Theme keeps only the section table;
// records, strings, pixels and glyphs are read through Source as they are
// drawn. Values holds every slot (about 1.5 KB) and Scratch the per-render
// buffers (about 2 KB) -- both fixed, whatever the theme's size.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace stv2 {

constexpr uint16_t None = 0xFFFF;
constexpr int Width = 240, Height = 240;
constexpr uint16_t FormatVersion = 1;
constexpr int MaxPath = 8;
constexpr size_t MaxText = 96;      // bytes of UTF-8 per text op
constexpr size_t PoolSize = 1024;   // string slot storage
constexpr size_t MaxSlots = 64;
constexpr uint32_t MaxFile = 900u * 1024u;

enum Sec : uint8_t {
  SecStrings = 1, SecScreens, SecOps, SecTemplates, SecParts, SecSlots, SecFormats,
  SecTables, SecEntries, SecSources, SecFields, SecImages, SecFonts, SecGlyphs, SecBlob, SecCount
};
enum OpType : uint8_t { OpRect = 1, OpCircle = 2, OpImage = 3, OpText = 4 };
enum OpFlag : uint8_t { FrameValue = 1, Animate = 2 };
enum SlotKind : uint8_t { SlotClock = 0, SlotCity = 1, SlotData = 2 };
enum SlotType : uint8_t { TypeNumber = 0, TypeString = 1 };
enum Clock : uint8_t { ClockH24, ClockH12, ClockMin, ClockSec, ClockDay, ClockMonth, ClockYear, ClockWeekday, ClockAmPm };

class Source {
 public:
  virtual ~Source() {}
  virtual uint32_t size() const = 0;
  virtual bool read(uint32_t offset, void* dest, size_t count) = 0;
};

struct StringRec { uint32_t offset; uint16_t length; };
struct Screen { uint16_t firstOp, opCount, seconds, background; };
struct Op { uint8_t type, flags; int16_t x, y, w, h; uint16_t color, color2, a, b, c; };
struct Template { uint16_t firstPart, partCount; };
struct Part { uint8_t kind; uint16_t a, b; };
struct Slot { uint8_t kind, type; uint16_t arg, poolOffset, poolSize; };
struct Format { int16_t add; uint8_t decimals, width, mod; uint16_t table; };
struct Table { uint16_t firstEntry, count, fallback; };
struct Entry { int16_t from, to; uint16_t value; };
struct SourceRec { uint16_t url, seconds, firstField, fieldCount; };
struct Field { uint16_t slot; uint8_t depth; uint32_t path[MaxPath]; };
struct Image { uint16_t w, h, frames; uint8_t encoding, flags; uint32_t offset, size; };
struct Font { uint16_t firstGlyph, glyphCount; uint8_t bpp, lineHeight, ascent; uint16_t fallback; uint32_t offset; };
struct Glyph { uint16_t codepoint; uint8_t w, h; int8_t x, y; uint8_t advance; uint32_t offset; };

// A validated theme file. open() checks everything the renderer later relies
// on (the CRC, every reference and range, every RLE row), so drawing needs no
// bounds checks of its own beyond read() failing.
class Theme {
 public:
  bool open(Source& source, char* error, size_t errorSize);
  void close() { source_ = nullptr; }
  bool isOpen() const { return source_ != nullptr; }
  uint16_t count(Sec s) const { return count_[s]; }
  uint16_t nameString() const { return name_; }

  bool screen(uint16_t i, Screen& out);
  bool op(uint16_t i, Op& out);
  bool templ(uint16_t i, Template& out);
  bool part(uint16_t i, Part& out);
  bool slot(uint16_t i, Slot& out);
  bool format(uint16_t i, Format& out);
  bool table(uint16_t i, Table& out);
  bool entry(uint16_t i, Entry& out);
  bool sourceRec(uint16_t i, SourceRec& out);
  bool field(uint16_t i, Field& out);
  bool image(uint16_t i, Image& out);
  bool font(uint16_t i, Font& out);
  bool glyph(uint16_t i, Glyph& out);
  bool stringRec(uint16_t i, StringRec& out);
  bool blob(uint32_t offset, void* dest, size_t count);
  // Copies string `i` into out (NUL-terminated, cut to cap - 1 bytes).
  size_t string(uint16_t i, char* out, size_t cap);
  // Table lookup: the first entry with from <= n <= to, else the fallback.
  uint16_t lookup(uint16_t tableIndex, long n);

 private:
  bool record(Sec s, uint16_t i, uint8_t* buf);
  bool validate(char* error, size_t errorSize);
  bool validateImage(const Image& im, char* error, size_t errorSize);
  Source* source_ = nullptr;
  uint32_t offset_[SecCount] = {};
  uint16_t count_[SecCount] = {};
  uint32_t blobSize_ = 0;
  uint16_t name_ = None;
};

// Every slot's current value. Numbers are doubles (the JSON values and the
// browser's preview agree on them); strings live in a fixed pool at the
// offset the compiler assigned.
struct Values {
  double number[MaxSlots];
  uint64_t has = 0;        // bit i: slot i holds a value
  char pool[PoolSize];
  void clear() { has = 0; }
  bool hasValue(uint16_t i) const { return i < MaxSlots && (has >> i) & 1; }
  void setNumber(uint16_t i, double v) { if (i < MaxSlots) { number[i] = v; has |= uint64_t(1) << i; } }
  void unset(uint16_t i) { if (i < MaxSlots) has &= ~(uint64_t(1) << i); }
  // Stores up to slot.poolSize bytes, cut on a UTF-8 boundary.
  void setString(uint16_t i, const Slot& slot, const char* text, size_t len);
};

// Fills the clock slots from broken-down local time (null: not synced) and
// the city slot from `city` (null or empty: unset).
struct ClockTime { int year, month, day, hour, minute, second, weekday; };
void applyClock(Theme& theme, Values& values, const ClockTime* time, const char* city);

// Formats slot `slot` through format `fmt` (None: plain) and appends it to
// out; "--" when the slot holds nothing. Returns false only on a read error.
bool appendValue(Theme& theme, const Values& values, uint16_t slot, uint16_t fmt, char* out, size_t cap, size_t& len);
// Resolves template `t` into out (at most MaxText bytes, cut on a character boundary).
bool resolveText(Theme& theme, const Values& values, uint16_t t, char* out, size_t cap);

// Per-render working memory, allocated once while a theme is loaded.
struct Scratch {
  uint16_t colors[Width];
  uint8_t alpha[Width];
  uint8_t rle[2 * Width + 4];          // worst case: 2 bytes per pixel
  uint16_t palette[256];
  uint8_t paletteAlpha[256];
  uint16_t paletteImage = None;         // image whose palette is cached
  struct CachedGlyph { uint16_t font; uint16_t codepoint; Glyph glyph; };
  CachedGlyph glyphs[32];
  uint8_t glyphValid[32 / 8];
  char text[MaxText + 64];
  void reset() { paletteImage = None; for (auto& b : glyphValid) b = 0; }
};

// Draws rows y0 .. y0 + rows - 1 of screen `screenIndex` into band
// (Width * rows pixels, RGB565). `timeMs` drives animations.
bool renderBand(Theme& theme, const Values& values, uint16_t screenIndex, uint32_t timeMs,
                int y0, int rows, uint16_t* band, Scratch& scratch);
// The frame an image op shows now (shared with the device's dirty tracking).
uint16_t imageFrame(Theme& theme, const Values& values, const Op& op, uint32_t timeMs);
// Glyph lookup with the scratch cache; falls back to the font's fallback glyph.
bool findGlyph(Theme& theme, Scratch& scratch, uint16_t fontIndex, const Font& font, uint32_t codepoint, Glyph& out);

// Streaming JSON reader for one source's response: feed() any number of byte
// chunks; each scalar whose path matches one of the source's fields is
// written into its slot. Memory: the path stack and up to 64 field hashes.
class JsonMatcher {
 public:
  bool begin(Theme& theme, Values& values, uint16_t sourceIndex);
  void feed(const uint8_t* data, size_t n);
  uint16_t matches() const { return matches_; }
 private:
  void scalarDone();
  void push(bool isArray);
  void pop();
  void beginValue();
  uint32_t pathHash() const;
  bool matchField(uint16_t& slot);
  void storeString();
  Theme* theme_ = nullptr;
  Values* values_ = nullptr;
  struct FieldKey { uint32_t hash; uint16_t index; uint8_t depth; };
  FieldKey keys_[64];
  uint8_t keyCount_ = 0;
  uint16_t firstField_ = 0;
  uint16_t matches_ = 0;
  // Container stack: bit d of arrays_ set = level d is an array.
  uint32_t arrays_ = 0;
  uint8_t depth_ = 0, overflow_ = 0;
  uint32_t tokens_[MaxPath];
  uint32_t index_[MaxPath];
  enum State : uint8_t { Value, Key, AfterKey, AfterValue, InString, InKeyString, Escape, KeyEscape, Unicode, KeyUnicode, InNumber, InLiteral };
  State state_ = Value;
  bool expectKey_ = false;
  uint32_t keyHash_ = 0;
  uint16_t unicode_ = 0;
  uint8_t unicodeDigits_ = 0;
  char buf_[40];
  uint8_t bufLen_ = 0;
  bool bufOverflow_ = false;
  // String value capture, for string slots only.
  char str_[256];
  uint16_t strLen_ = 0;
};

// Expands {lat}/{lon} in `url` (four decimals). Returns false if the URL needs
// a location and none is set (lat == lon == 0), or if it does not fit.
bool expandUrl(const char* url, double lat, double lon, char* out, size_t cap);
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t n);

}  // namespace stv2
