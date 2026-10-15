#pragma once
// Portable wire-format pieces of the companion protocol (no Arduino, no
// filesystem), so the native tests in tests/companion/ cover exactly what the
// device runs. See docs/.../features/companion.md for the protocol itself.
#include <cstddef>
#include <cstdint>

namespace companion {
constexpr int Panel = 240;

// A pushed rectangle must lie entirely on the panel; nothing is clipped, so a
// companion bug shows up as a 400 instead of a half-drawn frame.
bool validRect(long x, long y, long w, long h);

// Reassembles little-endian RGB565 pixels from a byte stream that the web
// server splits at arbitrary points (a pixel can straddle two chunks), and
// hands them to the panel a few whole rows at a time.
class PixelStream {
 public:
  // `buf` holds `cap` pixels; at least one row (w) must fit.
  void begin(int w, int h, uint16_t* buf, size_t cap);
  // Feeds bytes; calls sink(rowOffset, pixels, rows) for each batch of full
  // rows. Returns false once more bytes arrive than w*h*2.
  template <class Sink> bool feed(const uint8_t* p, size_t n, Sink sink) {
    for (size_t i = 0; i < n; ++i) {
      if (received_ >= total_) return false;
      ++received_;
      if (!half_) { low_ = p[i]; half_ = true; continue; }
      half_ = false;
      buf_[fill_++] = (uint16_t)(low_ | (p[i] << 8));
      if (fill_ == batch_) flush(sink);
    }
    return true;
  }
  // Flushes a final partial batch (always whole rows when complete()).
  template <class Sink> void finish(Sink sink) { if (fill_) flush(sink); }
  bool complete() const { return received_ == total_; }

 private:
  template <class Sink> void flush(Sink sink) {
    int rows = (int)(fill_ / (size_t)w_);
    if (rows) sink(row_, buf_, rows);
    row_ += rows;
    fill_ = 0;
  }
  uint16_t* buf_ = nullptr;
  size_t batch_ = 0, fill_ = 0, received_ = 0, total_ = 0;
  int w_ = 1, row_ = 0;
  uint8_t low_ = 0;
  bool half_ = false;
};

// Digit atlas ("SCG1"): the companion pre-renders the clock's glyphs in any
// font, already blended over the clock background, so the device only copies
// pixels. Layout, little-endian:
//   0  "SCG1"
//   4  u8 count (= AtlasGlyphs), u8 height, u16 reserved
//   8  count x u8 width
//   8+count  glyph pixels, glyph after glyph, each width*height RGB565
constexpr int AtlasGlyphs = 11;               // "0123456789:"
constexpr size_t AtlasHeaderSize = 8 + AtlasGlyphs;
struct Atlas {
  uint8_t height = 0;
  uint8_t widths[AtlasGlyphs] = {};
  uint32_t offsets[AtlasGlyphs] = {};         // file offset of each glyph
};
// Index of `c` in the atlas, or -1.
int atlasIndex(char c);
// Validates the header against the whole file size; fills `out` on success.
bool parseAtlas(const uint8_t* header, size_t headerLen, size_t fileSize, Atlas& out);
}  // namespace companion
