#include "CompanionProto.h"
#include <cstring>

namespace companion {
bool validRect(long x, long y, long w, long h) {
  return x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= Panel && y + h <= Panel;
}

void PixelStream::begin(int w, int h, uint16_t* buf, size_t cap) {
  w_ = w > 0 ? w : 1;
  buf_ = buf;
  size_t rows = cap / (size_t)w_;
  batch_ = (rows ? rows : 1) * (size_t)w_;
  total_ = (size_t)w_ * (size_t)(h > 0 ? h : 0) * 2;
  fill_ = received_ = 0;
  row_ = 0;
  half_ = false;
}

int atlasIndex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  return c == ':' ? 10 : -1;
}

bool parseAtlas(const uint8_t* h, size_t len, size_t fileSize, Atlas& out) {
  if (len < AtlasHeaderSize || memcmp(h, "SCG1", 4) != 0) return false;
  if (h[4] != AtlasGlyphs || h[5] == 0 || h[5] > Panel) return false;
  Atlas a;
  a.height = h[5];
  uint32_t off = AtlasHeaderSize;
  for (int i = 0; i < AtlasGlyphs; ++i) {
    uint8_t w = h[8 + i];
    if (w == 0 || w > Panel) return false;
    a.widths[i] = w;
    a.offsets[i] = off;
    off += (uint32_t)w * a.height * 2;
  }
  if (off != fileSize) return false;
  out = a;
  return true;
}
}  // namespace companion
