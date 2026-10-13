#include "Theme2Mode.h"
#if WITH_THEME2
#include "Clock.h"
#include "Gfx.h"
#include "Platform.h"
#include "features/theme/ThemeMode.h"
#include <Arduino_GFX_Library.h>
#include <new>

Theme2Mode g_theme2Mode;

namespace {
constexpr int BandRows = 4;
constexpr uint32_t FetchTimeoutMs = 6000;
constexpr uint16_t MaxOps = 96;
}

// Everything a loaded theme needs at run time, in one block so loading and
// unloading never fragment the heap.
struct Theme2Mode::Work {
  stv2::Values values;
  stv2::Scratch scratch;
  stv2::JsonMatcher json;
  uint16_t band[stv2::Width * BandRows];
  uint32_t opHash[MaxOps];     // what each op of the current screen last showed
  uint8_t dirty[stv2::Height / BandRows];
};

void Theme2Mode::begin(const Settings&) {
  LittleFS.mkdir(theme2::Dir);
  LittleFS.remove(theme2::Staging);   // an interrupted upload
}

void Theme2Mode::unload() {
  delete work_;
  work_ = nullptr;
  theme_.close();
  file_.close();
  attempted_ = messageDrawn_ = false;
  full_ = true;
  error_ = name_ = "";
  screens_ = screen_ = 0;
  themeCrashGuard(false);
}

void Theme2Mode::invalidate(const Settings& s) {
  if (s.mode != MODE_THEME2) unload();
  full_ = true;
  messageDrawn_ = false;
}

bool Theme2Mode::load(const Settings&) {
  // Armed from here until unload(): a theme that crashes the device is turned
  // off at the next boot (see themeRecoverAfterCrash).
  themeCrashGuard(true);
  char err[64] = "";
  if (!file_.open(theme2::Active)) { error_ = "No theme installed yet"; themeCrashGuard(false); return false; }
  if (!theme_.open(file_, err, sizeof err)) {
    error_ = err;
    file_.close();
    themeCrashGuard(false);
    return false;
  }
  work_ = new (std::nothrow) Work;
  if (!work_) {
    error_ = "Not enough memory";
    theme_.close();
    file_.close();
    themeCrashGuard(false);
    return false;
  }
  work_->values.clear();
  work_->scratch.reset();
  char name[64];
  theme_.string(theme_.nameString(), name, sizeof name);
  name_ = name;
  screens_ = theme_.count(stv2::SecScreens);
  screen_ = 0;
  started_ = shownAt_ = millis();
  // Animated ops need a 100 ms tick; static themes only change with values.
  animated_ = false;
  for (uint16_t i = 0; i < theme_.count(stv2::SecOps); i++) {
    stv2::Op op;
    if (theme_.op(i, op) && op.type == stv2::OpImage && (op.flags & stv2::Animate)) animated_ = true;
  }
  for (auto& t : nextFetch_) t = 0;
  fetchCursor_ = 0;
  full_ = true;
  error_ = "";
  return true;
}

void Theme2Mode::updateValues(const Settings& s) {
  tm t{};
  stv2::ClockTime ct;
  bool synced = clockNow(t);
  if (synced) ct = {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, t.tm_wday};
  stv2::applyClock(theme_, work_->values, synced ? &ct : nullptr, s.location.city.c_str());
}

// One source per call, the next one due in round-robin order. The fetch is
// synchronous and bounded; the response streams through the JSON reader, so
// its size costs nothing.
void Theme2Mode::fetchDue(const Settings& s) {
  uint16_t n = theme_.count(stv2::SecSources);
  if (!n || WiFi.status() != WL_CONNECTED) return;
  for (uint16_t k = 0; k < n; k++) {
    uint16_t i = (fetchCursor_ + k) % n;
    if (int32_t(millis() - nextFetch_[i]) < 0 && nextFetch_[i]) continue;
    fetchCursor_ = uint8_t((i + 1) % n);
    stv2::SourceRec src;
    if (!theme_.sourceRec(i, src)) return;
    nextFetch_[i] = millis() + uint32_t(src.seconds) * 1000UL;
    char url[256], expanded[288];
    theme_.string(src.url, url, sizeof url);
    if (!stv2::expandUrl(url, s.location.lat, s.location.lon, expanded, sizeof expanded)) return;
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(FetchTimeoutMs);
    http.setReuse(false);
    http.useHTTP10(true);
    if (!http.begin(client, expanded)) return;
    http.addHeader("Accept", "application/json");
    if (http.GET() == HTTP_CODE_OK && work_->json.begin(theme_, work_->values, i)) {
      Stream& body = http.getStream();
      int remaining = http.getSize();   // -1 when the server does not say
      uint8_t buf[128];
      uint32_t deadline = millis() + FetchTimeoutMs;
      while (int32_t(millis() - deadline) < 0 && remaining != 0) {
        int avail = body.available();
        if (avail <= 0) {
          if (!client.connected()) break;
          delay(2);
          continue;
        }
        int want = avail < int(sizeof buf) ? avail : int(sizeof buf);
        if (remaining > 0 && want > remaining) want = remaining;
        int got = body.readBytes(buf, want);
        if (got <= 0) break;
        work_->json.feed(buf, size_t(got));
        if (remaining > 0) remaining -= got;
        yield();
      }
    }
    http.end();
    return;
  }
}

// Recomputes what each op of the current screen shows (its text, or its
// frame) and marks the bands under any op whose content changed.
void Theme2Mode::markDirty() {
  stv2::Screen sc;
  if (!theme_.screen(screen_, sc)) return;
  if (full_) {
    for (auto& d : work_->dirty) d = 1;
    for (auto& h : work_->opHash) h = 0;
  }
  uint32_t now = millis() - started_;
  for (uint16_t k = 0; k < sc.opCount && k < MaxOps; k++) {
    stv2::Op op;
    if (!theme_.op(sc.firstOp + k, op)) return;
    uint32_t h = 1;
    if (op.type == stv2::OpText) {
      stv2::resolveText(theme_, work_->values, op.b, work_->scratch.text, sizeof work_->scratch.text);
      h = 0x811C9DC5u;
      for (const char* p = work_->scratch.text; *p; p++) h = (h ^ uint8_t(*p)) * 0x01000193u;
    } else if (op.type == stv2::OpImage) {
      h = 2 + stv2::imageFrame(theme_, work_->values, op, now);
    }
    if (h == work_->opHash[k] && !full_) continue;
    work_->opHash[k] = h;
    int height = op.type == stv2::OpCircle ? op.w : op.h;
    int y0 = op.y < 0 ? 0 : op.y, y1 = op.y + height > stv2::Height ? stv2::Height : op.y + height;
    for (int y = y0; y < y1; y += 1) work_->dirty[y / BandRows] = 1;
  }
  full_ = false;
}

void Theme2Mode::draw() {
  uint32_t now = millis() - started_;
  Arduino_GFX* gfx = gfxDev();
  for (int b = 0; b < stv2::Height / BandRows; b++) {
    if (!work_->dirty[b]) continue;
    work_->dirty[b] = 0;
    if (!stv2::renderBand(theme_, work_->values, screen_, now, b * BandRows, BandRows, work_->band, work_->scratch)) {
      error_ = "Could not read the theme file";
      return;
    }
    for (int i = 0; i < stv2::Width * BandRows; i++) work_->band[i] = gfxTint(work_->band[i]);
    if (gfx) gfx->draw16bitRGBBitmap(0, b * BandRows, work_->band, stv2::Width, BandRows);
    yield();
  }
}

void Theme2Mode::service(const Settings& s) {
  if (!work_ && !attempted_) {
    attempted_ = true;
    load(s);
  }
  if (!work_) {
    if (!messageDrawn_) {
      gfxMessage("THEME", error_.c_str(), C_WHITE);
      messageDrawn_ = true;
    }
    return;
  }
  // Screens rotate on their own durations.
  stv2::Screen sc;
  if (screens_ > 1 && theme_.screen(screen_, sc) && sc.seconds && millis() - shownAt_ >= uint32_t(sc.seconds) * 1000UL) {
    screen_ = uint16_t((screen_ + 1) % screens_);
    shownAt_ = millis();
    full_ = true;
  }
  if (!full_ && millis() - lastTick_ < (animated_ ? 100u : 250u)) return;
  lastTick_ = millis();
  updateValues(s);
  fetchDue(s);
  markDirty();
  draw();
}

bool Theme2Mode::snapshot(uint16_t index, void (*sink)(const uint16_t*, int, int, void*), void* ctx) {
  if (!work_ || index >= screens_) return false;
  uint32_t now = millis() - started_;
  for (int y = 0; y < stv2::Height; y += BandRows) {
    if (!stv2::renderBand(theme_, work_->values, index, now, y, BandRows, work_->band, work_->scratch)) return false;
    sink(work_->band, y, BandRows, ctx);
    yield();
  }
  // The band buffer was shared with the panel: repaint it on the next tick.
  full_ = true;
  return true;
}
#endif
