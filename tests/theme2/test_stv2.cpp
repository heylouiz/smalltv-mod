// Host tests for the theme v2 core: the device renderer must reproduce the
// browser's reference frames pixel for pixel, derive the same slot values
// from the clock and a streamed JSON response, resolve the same texts, and
// reject every damaged file. Fixtures come from tests/theme2/gen.mjs.
#include "Stv2.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace stv2;

static std::string dir;
static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); std::fprintf(stderr, __VA_ARGS__); std::fprintf(stderr, "\n"); failures++; } } while (0)

static std::vector<uint8_t> load(const std::string& name) {
  std::ifstream f(dir + "/" + name, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
static std::vector<std::string> lines(const std::string& name) {
  std::ifstream f(dir + "/" + name);
  std::vector<std::string> out;
  for (std::string l; std::getline(f, l);) if (!l.empty()) out.push_back(l);
  return out;
}
static std::string unhex(const std::string& h) {
  if (h == "-") return "";
  std::string s;
  for (size_t i = 0; i + 1 < h.size(); i += 2) s.push_back(char(std::strtol(h.substr(i, 2).c_str(), nullptr, 16)));
  return s;
}

// Reads through a counter so tests can see how much the renderer touches.
class MemorySource : public Source {
 public:
  explicit MemorySource(std::vector<uint8_t> d) : data(std::move(d)) {}
  uint32_t size() const override { return uint32_t(data.size()); }
  bool read(uint32_t offset, void* dest, size_t count) override {
    if (offset + count > data.size()) return false;
    std::memcpy(dest, data.data() + offset, count);
    reads++;
    return true;
  }
  std::vector<uint8_t> data;
  size_t reads = 0;
};

// The JS side's values, as gen.mjs wrote them.
static void loadValues(Theme& theme, Values& v, const std::string& name) {
  v.clear();
  for (const auto& l : lines(name + ".values")) {
    std::istringstream in(l);
    int slot; std::string kind, rest;
    in >> slot >> kind >> rest;
    if (kind == "n") v.setNumber(uint16_t(slot), std::strtod(rest.c_str(), nullptr));
    else if (kind == "s") { Slot s; theme.slot(uint16_t(slot), s); std::string t = unhex(rest); v.setString(uint16_t(slot), s, t.data(), t.size()); }
  }
}

static bool sameValue(Theme& theme, const Values& a, const Values& b, uint16_t i) {
  if (a.hasValue(i) != b.hasValue(i)) return false;
  if (!a.hasValue(i)) return true;
  Slot s; theme.slot(i, s);
  if (s.type == TypeNumber) return a.number[i] == b.number[i];
  size_t n = size_t(a.number[i]);
  return n == size_t(b.number[i]) && !std::memcmp(a.pool + s.poolOffset, b.pool + s.poolOffset, n);
}

static void renderCase(const std::string& name, int screens, uint32_t timeMs) {
  MemorySource src(load(name + ".stv"));
  Theme theme;
  char error[64] = "";
  CHECK(theme.open(src, error, sizeof error), "%s: open failed: %s", name.c_str(), error);
  if (!theme.isOpen()) return;
  static Values values;
  static Scratch scratch;
  loadValues(theme, values, name);
  scratch.reset();
  // Texts first: the clearest message when formatting drifts.
  auto texts = lines(name + ".texts");
  size_t t = 0;
  for (uint16_t i = 0; i < theme.count(SecOps); i++) {
    Op op; theme.op(i, op);
    if (op.type != OpText) continue;
    char buf[MaxText + 64];
    resolveText(theme, values, op.b, buf, sizeof buf);
    std::string want = t < texts.size() ? unhex(texts[t]) : "?";
    CHECK(want == buf, "%s: text op %u: got '%s', want '%s'", name.c_str(), i, buf, want.c_str());
    t++;
  }
  // Every screen, drawn in 8-row bands exactly as the device does.
  for (int s = 0; s < screens; s++) {
    auto expected = load(name + ".screen" + std::to_string(s) + ".rgb565");
    std::vector<uint16_t> frame(Width * Height);
    uint16_t band[Width * 8];
    for (int y = 0; y < Height; y += 8) {
      CHECK(renderBand(theme, values, uint16_t(s), timeMs, y, 8, band, scratch), "%s: band %d failed", name.c_str(), y);
      std::memcpy(&frame[y * Width], band, sizeof band);
    }
    int diffs = 0, first = -1;
    for (int p = 0; p < Width * Height; p++) {
      uint16_t want = uint16_t(expected[2 * p] | (expected[2 * p + 1] << 8));
      if (frame[p] != want) { if (first < 0) first = p; diffs++; }
    }
    CHECK(diffs == 0, "%s screen %d: %d pixels differ, first at (%d,%d): got %04x want %04x", name.c_str(), s, diffs,
          first % Width, first / Width, first < 0 ? 0 : frame[first],
          first < 0 ? 0 : uint16_t(expected[2 * first] | (expected[2 * first + 1] << 8)));
  }
  std::printf("%s: %d screen(s) match, %zu flash reads\n", name.c_str(), screens, src.reads);
}

// Clock + city through applyClock, data through JsonMatcher fed in uneven
// chunks: must equal the values the browser computed.
static void valuesCase() {
  MemorySource src(load("sample.stv"));
  Theme theme;
  char error[64];
  if (!theme.open(src, error, sizeof error)) { CHECK(false, "sample: %s", error); return; }
  static Values want, got;
  loadValues(theme, want, "sample");
  auto json = load("forecast.json");
  for (size_t chunk : {size_t(1), size_t(3), size_t(17), size_t(4096)}) {
    got.clear();
    ClockTime time{2026, 10, 13, 9, 5, 7, 2};
    applyClock(theme, got, &time, "São José dos Campos");
    static JsonMatcher m;
    CHECK(m.begin(theme, got, 0), "matcher begin");
    for (size_t at = 0; at < json.size(); at += chunk) m.feed(json.data() + at, std::min(chunk, json.size() - at));
    for (uint16_t i = 0; i < theme.count(SecSlots); i++)
      CHECK(sameValue(theme, got, want, i), "slot %u differs (chunk %zu)", i, chunk);
    CHECK(m.matches() == 6, "matches %u (chunk %zu)", m.matches(), chunk);
  }
  // Escapes, nesting beyond the path, a same-named key elsewhere, literals.
  got.clear();
  static JsonMatcher m;
  m.begin(theme, got, 0);
  const char* tricky =
      "{\"other\":{\"temperature_2m\":99},\"cur\\u0072ent\":{\"deep\":[[[[[[[[[1]]]]]]]]],"
      "\"temperature_2m\":-3.5e1,\"weather_code\":true},\"daily\":{\"temperature_2m_max\":[null,\"x\",7]}}";
  m.feed(reinterpret_cast<const uint8_t*>(tricky), std::strlen(tricky));
  Field f;
  for (uint16_t i = 0; i < theme.count(SecFields); i++) {
    theme.field(i, f);
    (void)f;
  }
  // current.temperature_2m is slot of field 0, weather_code field 1 (compile order).
  Field temp, code;
  theme.field(0, temp);
  theme.field(1, code);
  CHECK(got.hasValue(temp.slot) && got.number[temp.slot] == -35, "escaped key + exponent");
  CHECK(got.hasValue(code.slot) && got.number[code.slot] == 1, "true as 1");
}

static void urlCase() {
  char out[128];
  CHECK(expandUrl("http://x/?lat={lat}&lon={lon}", -22.56474, -47.40171, out, sizeof out), "expand");
  CHECK(!std::strcmp(out, "http://x/?lat=-22.5647&lon=-47.4017"), "url %s", out);
  CHECK(!expandUrl("http://x/?lat={lat}", 0, 0, out, sizeof out), "no location");
  CHECK(expandUrl("http://x/plain", 0, 0, out, sizeof out) && !std::strcmp(out, "http://x/plain"), "plain url");
  CHECK(!expandUrl("http://x/?lat={lat}", 1, 1, out, 12), "does not fit");
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: %s <fixtures-dir>\n", argv[0]); return 2; }
  dir = argv[1];
  for (const auto& l : lines("cases.txt")) {
    std::istringstream in(l);
    std::string name; int screens; uint32_t timeMs;
    in >> name >> screens >> timeMs;
    renderCase(name, screens, timeMs);
  }
  valuesCase();
  urlCase();
  for (const auto& name : lines("bad.txt")) {
    MemorySource src(load(name + ".stv"));
    Theme theme;
    char error[64] = "";
    CHECK(!theme.open(src, error, sizeof error), "%s was accepted", name.c_str());
    std::printf("%s rejected: %s\n", name.c_str(), error);
  }
  std::printf(failures ? "theme2 tests FAILED (%d)\n" : "theme2 tests passed\n", failures);
  return failures ? 1 : 0;
}
