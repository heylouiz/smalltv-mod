#include "ThemeMode.h"
#if WITH_THEME
#include "Clock.h"
#include "Gfx.h"
#include "Net.h"
#include "Platform.h"
#include "ThemeDataClient.h"
#include <Arduino_GFX_Library.h>
#include <font/glcdfont.h>
#include <algorithm>

ThemeMode g_themeMode;
void themeCrashGuard(bool armed) {
  uint32_t keep = platformKeepRead();
  uint32_t next = armed ? keep | KEEP_THEME : keep & ~KEEP_THEME;
  if (next != keep) platformKeepWrite(next);
}
bool themeRecoverAfterCrash(Settings& s, uint32_t keep) {
  if (!(keep & KEEP_THEME)) return false;
  // Only an active theme is turned off: a crash while validating an upload
  // with another mode on screen leaves the saved selection alone. If the save
  // fails the fallback still holds for this boot, and a repeat crash lands
  // here again rather than in a loop.
  String id = s.mode == MODE_THEME ? s.themeId : String();
  if (s.mode == MODE_THEME2) {
    // The editor theme: back to theme mode on the built-in clock. The file
    // stays installed, to be fixed in the editor and sent again.
    id = "editor theme";
    s.mode = MODE_THEME;
    s.themeId = "";
    saveSettings(s);
  } else if (id.length()) {
    s.themeId = "";
    saveSettings(s);
  }
  g_themeMode.setRecovered(id);
  return true;
}
namespace {
// Adapts the portable engine's Display interface to this device's actual
// panel: glyphColumn() reads the built-in font from flash (PROGMEM), and
// row() applies the usual panel color tint before handing the row to the
// GFX driver. yield() every 16 rows keeps a large dirty region from starving
// WiFi/other tasks during a long render.
class Panel : public smalltv::Display {
 public:
  uint8_t glyphColumn(uint8_t c, uint8_t col) const override {
    return pgm_read_byte(font + unsigned(c) * 5 + col);
  }
  void row(int x, int y, const uint16_t* pixels, int count) override {
    uint16_t tinted[240];
    for (int i = 0; i < count; ++i) tinted[i] = gfxTint(pixels[i]);
    gfxDev()->draw16bitRGBBitmap(x, y, tinted, count, 1);
    if ((y & 15) == 15) yield();
  }
};
}
void ThemeMode::begin(const Settings&) {
  LittleFS.mkdir("/themes");
  // An interrupted install leaves its staging file behind; boot is the one
  // point it is safe to remove without racing a request in progress.
  LittleFS.remove("/themes/.upload");
}
// Drops every bit of state for the currently loaded theme -- the open
// package/file, any in-flight or fetched data, and the engine itself -- so
// the next service() call starts a clean load from scratch.
void ThemeMode::unload() {
  dataRequests_.cancel();
  dataRequests_.take();
  package_.reset();
  file_.reset();
  dataNextMs_.clear();
  dataScheduled_.clear();
  dataValues_.clear();
  dataCursor_ = 0;
  engine_.setValues({});
  engine_.setTheme(smalltv::Theme{}, millis());
  loadedId_ = "";
  attempted_ = messageDrawn_ = locationApplied_ = false;
  error_ = "";
  themeCrashGuard(false);
}
// The built-in theme: the time and the device's address, drawn straight with
// the built-in font. No package, no JSON, no heap -- it is what the device
// falls back to after a theme crash, so it must not be able to fail the same
// way. Repaints only what changed: the time each minute, the IP when it moves.
void ThemeMode::renderBuiltin() {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  if (!messageDrawn_) {
    gfx->fillScreen(C_BLACK);
    if (recovered_) {
      gfxDrawCentered("THEME TURNED OFF", 12, 2, C_YELLOW);
      String why = recoveredId_.length() ? recoveredId_ + " crashed" : String("theme crashed");
      gfxDrawCentered(why.c_str(), 34, gfxFitSize(why.c_str(), 232, 2), C_GRAY);
    }
    builtinMinute_ = -2;
    builtinIp_ = "";
    messageDrawn_ = true;
  }
  tm t{};
  int minute = clockNow(t) ? t.tm_hour * 60 + t.tm_min : -1;
  if (minute != builtinMinute_) {
    builtinMinute_ = minute;
    char hhmm[6];
    if (minute < 0) strcpy(hhmm, "--:--");
    else snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
    // Fixed width at a fixed spot, so an opaque redraw replaces the old time.
    gfx->setTextSize(7);
    gfx->setTextColor(C_WHITE, C_BLACK);
    gfx->setCursor((TFT_WIDTH - gfxTextW(hhmm, 7)) / 2, 84);
    gfx->print(hhmm);
  }
  String ip = netIP();
  if (ip != builtinIp_) {
    builtinIp_ = ip;
    gfx->fillRect(0, 170, TFT_WIDTH, 24, C_BLACK);
    const char* shown = ip.length() ? ip.c_str() : "-";
    gfxDrawCentered(shown, 170, gfxFitSize(shown, 232, 3), C_GREEN);
  }
}
// Mirrors the device location into the loaded theme: the city reaches text
// layers as the {CITY} value, and a moved location reschedules every source
// at once, since their URLs may embed {lat}/{lon}. Cheap when nothing changed.
void ThemeMode::applyLocation(const Settings& s) {
  smalltv::Location location;
  location.set = s.location.isSet();
  location.lat = s.location.lat;
  location.lon = s.location.lon;
  bool moved = location.set != location_.set || location.lat != location_.lat || location.lon != location_.lon;
  if (locationApplied_ && !moved && city_ == s.location.city) return;
  if (locationApplied_ && moved) dataScheduled_.assign(dataScheduled_.size(), false);
  location_ = location;
  city_ = s.location.city;
  locationApplied_ = true;
  auto city = std::find_if(dataValues_.begin(), dataValues_.end(), [](const smalltv::ThemeValue& v) { return v.key == smalltv::CityKey; });
  if (city_.length() == 0) {
    if (city != dataValues_.end()) dataValues_.erase(city);   // {CITY} falls back to "--"
  } else if (city == dataValues_.end()) {
    dataValues_.push_back({smalltv::CityKey, city_.c_str()});
  } else {
    city->value = city_.c_str();
  }
  engine_.setValues(dataValues_);
}
// Collects a finished fetch (if any) and, with at most one request in flight
// at a time, starts the next source whose interval has elapsed, round-robin
// over sources so no single slow/failing one starves the others.
void ThemeMode::refreshData() {
  const auto& sources = engine_.theme().data;
  if (auto result = dataRequests_.take()) {
    if (result->success) {
      // Merge into the running set by key rather than replace it wholesale:
      // a source's fields persist across refreshes of the *other* sources, so
      // a slow source does not blank fields a fast one already delivered.
      for (auto& value : result->values) {
        auto existing = std::find_if(dataValues_.begin(), dataValues_.end(), [&](const smalltv::ThemeValue& old) { return old.key == value.key; });
        if (existing == dataValues_.end()) dataValues_.push_back(std::move(value));
        else existing->value = std::move(value.value);
      }
      engine_.setValues(dataValues_);
    }
    // Schedule from completion, preserving the interval even for slow sources.
    dataNextMs_[result->index] = millis() + sources[result->index].interval * 1000UL;
  }
  if (sources.empty() || dataRequests_.busy() || WiFi.status() != WL_CONNECTED) return;
  // Scan forward from `dataCursor_` for the first source that is due (an
  // unscheduled one always is), start it, and advance the cursor past it --
  // so a source that just fired goes to the back of the round-robin queue.
  for (size_t offset = 0; offset < sources.size(); ++offset) {
    size_t i = (dataCursor_ + offset) % sources.size();
    if (dataScheduled_[i] && static_cast<int32_t>(millis() - dataNextMs_[i]) < 0) continue;
    auto request = dataRequests_.start(sources[i], i);
    // A URL that needs {lat}/{lon} with no location set is skipped like a
    // failed fetch; applyLocation() reschedules it once one is saved.
    std::string url;
    if (!smalltv::expandDataUrl(request->source.url, location_, url)) request->finish(false);
    else {
      request->source.url = std::move(url);
      if (!startThemeDataFetch(request)) request->finish(false);
    }
    dataScheduled_[i] = true;
    dataCursor_ = (i + 1) % sources.size();
    break;
  }
}
// Called when the theme should be redrawn from scratch, e.g. when a
// notification overlay just finished. Switches to a different theme by
// unloading first; otherwise leaves the loaded package alone and only marks
// the engine's next update() as a full repaint.
void ThemeMode::invalidate(const Settings& s) {
  if (loadedId_ != s.themeId || s.mode != MODE_THEME) unload();
  // A failed load may be retried after installation or settings changes.
  if (!package_) attempted_ = false;
  engine_.invalidate();
  messageDrawn_ = false;
}
// Per-frame entry point: loads the selected theme at most once per selection
// (via `attempted_`, so a failed load is not retried every call -- only on
// the next invalidate()/selection change), then renders it. `attempted_` and
// `loadedId_` together are the load state machine; everything past the load
// block runs every call once a package is open.
void ThemeMode::service(const Settings& s) {
  if (loadedId_ != s.themeId) unload();
  if (s.themeId.isEmpty()) {
    renderBuiltin();
    return;
  }
  if (!attempted_) {
    attempted_ = true;
    loadedId_ = s.themeId;
    // Armed from here until unload(): the load, every render and fetch, and
    // the heap the theme keeps while it is on screen.
    themeCrashGuard(true);
    std::string error;
    smalltv::Theme theme;
    if (!smalltv::validId(s.themeId.c_str())) error = "Select an installed theme";
    else {
      file_.reset(new ThemeFile);
      if (!file_->open("/themes/" + s.themeId + ".stheme")) error = "Theme file not found";
      else {
        package_.reset(new smalltv::Package(*file_));
        if (!package_->load(theme, error)) package_.reset();
        else if (theme.id != s.themeId.c_str()) {
          error = "Theme ID mismatch";
          package_.reset();
        }
      }
    }
    if (!error.empty()) {
      error_ = error.c_str();
      package_.reset();
      file_.reset();
      themeCrashGuard(false);
    } else {
      dataNextMs_.assign(theme.data.size(), 0);
      dataScheduled_.assign(theme.data.size(), false);
      dataValues_.clear();
      dataCursor_ = 0;
      locationApplied_ = false;
      engine_.setTheme(std::move(theme), millis());
      engine_.setValues(dataValues_);
      error_ = "";
    }
  }
  if (!package_) {
    if (!messageDrawn_) {
      gfxMessage("THEMES", error_.c_str(), C_WHITE);
      messageDrawn_ = true;
    }
    return;
  }
  applyLocation(s);
  refreshData();
  tm t{};
  bool valid = clockNow(t);
  Panel panel;
  auto dirty = engine_.update(millis(), valid ? &t : nullptr);
  // A render failure (a corrupt or truncated asset read mid-frame) is treated
  // like a load failure: drop the package and show an error rather than
  // risk repeatedly failing on every subsequent frame.
  if (!smalltv::render(engine_, dirty, *package_, panel)) {
    dataRequests_.cancel();
    dataRequests_.take();
    error_ = "Could not read theme asset";
    package_.reset();
    file_.reset();
    messageDrawn_ = false;
    themeCrashGuard(false);
  }
}
#endif
