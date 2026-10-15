// Native tests for the companion wire format (src/features/companion/CompanionProto).
#include "CompanionProto.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace companion;

static void testValidRect() {
  assert(validRect(0, 0, 240, 240));
  assert(validRect(239, 239, 1, 1));
  assert(!validRect(0, 0, 0, 10));
  assert(!validRect(-1, 0, 10, 10));
  assert(!validRect(1, 0, 240, 10));
  assert(!validRect(0, 200, 10, 41));
}

struct Batch { int row, rows; std::vector<uint16_t> px; };

// Splits the byte stream at every possible point size, including odd ones
// that cut a pixel in half, and checks every pixel lands on the right row.
static void testPixelStreamSplits() {
  const int w = 7, h = 5;
  std::vector<uint8_t> bytes;
  for (int i = 0; i < w * h; ++i) { bytes.push_back((uint8_t)(i * 3)); bytes.push_back((uint8_t)(i >> 8 | 0x80)); }
  for (size_t chunk = 1; chunk <= bytes.size(); ++chunk) {
    for (size_t cap : {7u, 10u, 14u, 1024u}) {
      std::vector<uint16_t> buf(cap);
      std::vector<Batch> got;
      auto sink = [&](int row, const uint16_t* p, int rows) { got.push_back({row, rows, std::vector<uint16_t>(p, p + rows * w)}); };
      PixelStream s;
      s.begin(w, h, buf.data(), cap);
      for (size_t off = 0; off < bytes.size(); off += chunk)
        assert(s.feed(bytes.data() + off, std::min(chunk, bytes.size() - off), sink));
      s.finish(sink);
      assert(s.complete());
      int next = 0;
      for (auto& b : got) {
        assert(b.row == next);
        for (int i = 0; i < b.rows * w; ++i) {
          int idx = b.row * w + i;
          assert(b.px[i] == (uint16_t)(bytes[idx * 2] | bytes[idx * 2 + 1] << 8));
        }
        next += b.rows;
      }
      assert(next == h);
    }
  }
}

static void testPixelStreamBounds() {
  uint16_t buf[16];
  auto sink = [](int, const uint16_t*, int) {};
  PixelStream s;
  s.begin(2, 2, buf, 16);
  uint8_t bytes[10] = {};
  assert(!s.feed(bytes, 10, sink));      // 8 expected
  s.begin(2, 2, buf, 16);
  assert(s.feed(bytes, 7, sink));
  assert(!s.complete());                 // one byte short
}

static std::vector<uint8_t> atlas(uint8_t h, uint8_t w) {
  std::vector<uint8_t> f = {'S', 'C', 'G', '1', AtlasGlyphs, h, 0, 0};
  for (int i = 0; i < AtlasGlyphs; ++i) f.push_back(w);
  f.resize(f.size() + (size_t)AtlasGlyphs * w * h * 2);
  return f;
}

static void testAtlas() {
  auto f = atlas(20, 12);
  Atlas a;
  assert(parseAtlas(f.data(), AtlasHeaderSize, f.size(), a));
  assert(a.height == 20 && a.widths[10] == 12);
  assert(a.offsets[0] == AtlasHeaderSize);
  assert(a.offsets[3] == AtlasHeaderSize + 3u * 12 * 20 * 2);
  assert(!parseAtlas(f.data(), AtlasHeaderSize, f.size() - 1, a));   // truncated
  assert(!parseAtlas(f.data(), AtlasHeaderSize - 1, f.size(), a));   // short header
  f[0] = 'X';
  assert(!parseAtlas(f.data(), AtlasHeaderSize, f.size(), a));
  auto z = atlas(20, 0);
  assert(!parseAtlas(z.data(), AtlasHeaderSize, z.size(), a));        // zero width
  assert(atlasIndex('0') == 0 && atlasIndex('9') == 9 && atlasIndex(':') == 10 && atlasIndex('-') == -1);
}

int main() {
  testValidRect();
  testPixelStreamSplits();
  testPixelStreamBounds();
  testAtlas();
  puts("companion proto tests passed");
}
