// Theme2Mode.h — display mode for theme v2 (STV2), built in the browser editor.
//
// One theme is installed at a time: /theme2/active.stv, with the one it
// replaced kept as /theme2/previous.stv. The file stays in flash; while the
// mode runs it holds a single fixed workspace (values, render scratch, an
// 8-row band, the JSON reader) -- about 10 KB, whatever the theme's size.
#pragma once
#include "config.h"
#if WITH_THEME2
#include "Mode.h"
#include "Stv2.h"
#include <LittleFS.h>

class Theme2File : public stv2::Source {
 public:
  bool open(const char* path) {
    file_ = LittleFS.open(path, "r");
    return bool(file_);
  }
  void close() { file_.close(); }
  uint32_t size() const override { return file_ ? file_.size() : 0; }
  bool read(uint32_t offset, void* dest, size_t count) override {
    return file_ && file_.seek(offset) && size_t(file_.read(static_cast<uint8_t*>(dest), count)) == count;
  }
 private:
  mutable File file_;
};

namespace theme2 {
constexpr const char* Dir = "/theme2";
constexpr const char* Active = "/theme2/active.stv";
constexpr const char* Previous = "/theme2/previous.stv";
constexpr const char* Staging = "/theme2/.upload";
constexpr const char* SourceJson = "/theme2/source.json";
}

class Theme2Mode : public DisplayMode {
 public:
  const char* id() const override { return "theme2"; }
  uint8_t modeConst() const override { return MODE_THEME2; }
  void begin(const Settings&) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override;
  void wake(const Settings&) override { full_ = true; messageDrawn_ = false; }

  // Drops the loaded theme so the next service() opens the file again.
  void reload() { unload(); }
  bool loaded() const { return work_ != nullptr; }
  const String& error() const { return error_; }
  const String& name() const { return name_; }
  uint16_t screenCount() const { return screens_; }
  uint16_t currentScreen() const { return screen_; }
  // Renders screen `index` band by band through `sink(band, y, rows)` with
  // the live values: the snapshot API, and the same code the panel uses.
  bool snapshot(uint16_t index, void (*sink)(const uint16_t*, int, int, void*), void* ctx);

 private:
  struct Work;
  bool load(const Settings& s);
  void unload();
  void updateValues(const Settings& s);
  void fetchDue(const Settings& s);
  void markDirty();
  void draw();
  Work* work_ = nullptr;
  Theme2File file_;
  stv2::Theme theme_;
  String error_, name_;
  bool attempted_ = false, messageDrawn_ = false, full_ = true;
  uint16_t screens_ = 0, screen_ = 0;
  uint32_t started_ = 0, shownAt_ = 0, lastTick_ = 0;
  bool animated_ = false;
  uint32_t nextFetch_[4] = {};
  uint8_t fetchCursor_ = 0;
};
extern Theme2Mode g_theme2Mode;
#endif
