// Stv2Json.cpp — a streaming JSON reader that never holds the document. It
// tracks the path of the value being read (object keys as FNV-1a hashes,
// array indexes) and, at each scalar, compares it with the source's fields.
// Same matching rules as applySource() in editor/src/values.js.
#include "Stv2.h"
#include <stdlib.h>
#include <string.h>

namespace stv2 {
namespace {
constexpr uint32_t FnvBasis = 0x811C9DC5u, FnvPrime = 0x01000193u;
inline uint32_t fnv(uint32_t h, uint8_t b) { return (h ^ b) * FnvPrime; }
uint32_t hashTokens(const uint32_t* tokens, uint8_t n) {
  uint32_t h = FnvBasis;
  for (uint8_t i = 0; i < n; i++)
    for (int k = 0; k < 4; k++) h = fnv(h, uint8_t(tokens[i] >> (8 * k)));
  return h;
}
inline bool isWhite(uint8_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
int hexValue(uint8_t c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
// UTF-8 bytes of a \uXXXX escape (surrogates become U+FFFD).
size_t encodeUtf8(uint16_t cp, uint8_t* out) {
  if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
  if (cp < 0x80) { out[0] = uint8_t(cp); return 1; }
  if (cp < 0x800) { out[0] = uint8_t(0xC0 | (cp >> 6)); out[1] = uint8_t(0x80 | (cp & 0x3F)); return 2; }
  out[0] = uint8_t(0xE0 | (cp >> 12)); out[1] = uint8_t(0x80 | ((cp >> 6) & 0x3F)); out[2] = uint8_t(0x80 | (cp & 0x3F));
  return 3;
}
uint8_t simpleEscape(uint8_t c) {
  switch (c) {
    case 'b': return '\b'; case 'f': return '\f'; case 'n': return '\n';
    case 'r': return '\r'; case 't': return '\t'; default: return c;   // " \ /
  }
}
}  // namespace

bool JsonMatcher::begin(Theme& theme, Values& values, uint16_t sourceIndex) {
  theme_ = &theme;
  values_ = &values;
  keyCount_ = 0;
  matches_ = 0;
  arrays_ = 0;
  depth_ = overflow_ = 0;
  state_ = Value;
  bufLen_ = 0;
  strLen_ = 0;
  SourceRec src;
  if (!theme.sourceRec(sourceIndex, src)) return false;
  firstField_ = src.firstField;
  for (uint16_t i = 0; i < src.fieldCount && keyCount_ < 64; i++) {
    Field f;
    if (!theme.field(src.firstField + i, f)) return false;
    keys_[keyCount_++] = {hashTokens(f.path, f.depth), uint16_t(src.firstField + i), f.depth};
  }
  return true;
}

uint32_t JsonMatcher::pathHash() const { return hashTokens(tokens_, depth_); }

void JsonMatcher::push(bool isArray) {
  if (depth_ >= 32) { overflow_ = 1; return; }
  if (isArray) arrays_ |= 1u << depth_; else arrays_ &= ~(1u << depth_);
  if (depth_ < MaxPath) {
    index_[depth_] = 0;
    tokens_[depth_] = isArray ? 0x80000000u : 0;
  }
  depth_++;
}
void JsonMatcher::pop() {
  if (depth_) depth_--;
}

// A scalar ended: buf_ holds a number or literal, or str_ a string (state
// tells which). Write it into every field whose path is the current one.
void JsonMatcher::scalarDone() {
  if (overflow_ || depth_ == 0 || depth_ > MaxPath) return;
  uint32_t h = pathHash();
  for (uint8_t k = 0; k < keyCount_; k++) {
    if (keys_[k].depth != depth_ || keys_[k].hash != h) continue;
    Field f;
    if (!theme_->field(keys_[k].index, f)) continue;
    bool same = true;
    for (uint8_t d = 0; d < depth_; d++) if (f.path[d] != tokens_[d]) same = false;
    if (!same) continue;
    Slot slot;
    if (!theme_->slot(f.slot, slot)) continue;
    if (state_ == InString) {
      if (slot.type != TypeString) continue;
      values_->setString(f.slot, slot, str_, strLen_);
    } else {
      if (slot.type != TypeNumber || bufOverflow_) continue;
      buf_[bufLen_] = 0;
      if (state_ == InLiteral) {
        if (!strcmp(buf_, "true")) values_->setNumber(f.slot, 1);
        else if (!strcmp(buf_, "false")) values_->setNumber(f.slot, 0);
        else continue;   // null
      } else {
        char* end = nullptr;
        double v = strtod(buf_, &end);
        if (end == buf_) continue;
        values_->setNumber(f.slot, v);
      }
    }
    matches_++;
  }
}

void JsonMatcher::feed(const uint8_t* data, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = data[i];
  again:
    switch (state_) {
      case Value:
        if (isWhite(c)) break;
        if (c == '{') { push(false); state_ = Key; break; }
        if (c == '[') { push(true); break; }               // next byte starts element 0 (or ']')
        if (c == ']') { pop(); state_ = AfterValue; break; } // empty array
        if (c == '"') { state_ = InString; strLen_ = 0; break; }
        bufLen_ = 0; bufOverflow_ = false;
        state_ = (c == '-' || (c >= '0' && c <= '9')) ? InNumber : InLiteral;
        goto again;
      case Key:
        if (isWhite(c) || c == ',') break;
        if (c == '}') { pop(); state_ = AfterValue; break; }
        if (c == '"') { state_ = InKeyString; keyHash_ = FnvBasis; }
        break;
      case InKeyString:
        if (c == '\\') { state_ = KeyEscape; break; }
        if (c == '"') {
          if (depth_ && depth_ <= MaxPath) tokens_[depth_ - 1] = keyHash_ & 0x7FFFFFFFu;
          state_ = AfterKey;
          break;
        }
        keyHash_ = fnv(keyHash_, c);
        break;
      case KeyEscape:
        if (c == 'u') { state_ = KeyUnicode; unicode_ = 0; unicodeDigits_ = 0; break; }
        keyHash_ = fnv(keyHash_, simpleEscape(c));
        state_ = InKeyString;
        break;
      case KeyUnicode: {
        int v = hexValue(c);
        unicode_ = uint16_t((unicode_ << 4) | (v < 0 ? 0 : v));
        if (++unicodeDigits_ == 4) {
          uint8_t u[3];
          size_t len = encodeUtf8(unicode_, u);
          for (size_t k = 0; k < len; k++) keyHash_ = fnv(keyHash_, u[k]);
          state_ = InKeyString;
        }
        break;
      }
      case AfterKey:
        if (c == ':') state_ = Value;
        break;
      case InString:
        if (c == '\\') { state_ = Escape; break; }
        if (c == '"') { scalarDone(); state_ = AfterValue; break; }
        if (strLen_ < sizeof(str_)) str_[strLen_++] = char(c);
        break;
      case Escape:
        if (c == 'u') { state_ = Unicode; unicode_ = 0; unicodeDigits_ = 0; break; }
        if (strLen_ < sizeof(str_)) str_[strLen_++] = char(simpleEscape(c));
        state_ = InString;
        break;
      case Unicode: {
        int v = hexValue(c);
        unicode_ = uint16_t((unicode_ << 4) | (v < 0 ? 0 : v));
        if (++unicodeDigits_ == 4) {
          uint8_t u[3];
          size_t len = encodeUtf8(unicode_, u);
          for (size_t k = 0; k < len && strLen_ < sizeof(str_); k++) str_[strLen_++] = char(u[k]);
          state_ = InString;
        }
        break;
      }
      case InNumber:
      case InLiteral:
        if ((state_ == InNumber && ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) ||
            (state_ == InLiteral && c >= 'a' && c <= 'z')) {
          if (bufLen_ < sizeof(buf_) - 1) buf_[bufLen_++] = char(c); else bufOverflow_ = true;
          break;
        }
        scalarDone();
        state_ = AfterValue;
        goto again;
      case AfterValue:
        if (isWhite(c)) break;
        if (c == ',') {
          if (depth_ && (arrays_ >> (depth_ - 1)) & 1) {
            if (depth_ <= MaxPath) tokens_[depth_ - 1] = 0x80000000u | ++index_[depth_ - 1];
            state_ = Value;
          } else state_ = Key;
          break;
        }
        if (c == '}' || c == ']') { pop(); break; }
        break;
    }
  }
}

}  // namespace stv2
