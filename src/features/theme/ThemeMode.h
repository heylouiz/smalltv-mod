#pragma once
#include "config.h"
#if WITH_THEME
#include "Mode.h"
#include "Platform.h"
#include "ThemePackage.h"
#include "ThemeData.h"
#include <LittleFS.h>
#include <memory>
#include <vector>

class ThemeFile : public smalltv::Source {
 public:
  bool open(const String& path) {
    file_ = LittleFS.open(path, "r");
    return bool(file_);
  }
  uint32_t size() const override { return file_ ? file_.size() : 0; }
  bool read(uint32_t offset, void* dest, size_t count) override {
    return file_ && file_.seek(offset) && file_.read(static_cast<uint8_t*>(dest), count) == count;
  }
 private:
  File file_;
};
// Crash recovery. While a user theme is active -- or while a package is being
// validated -- the KEEP_THEME breadcrumb is set, so if the chip crashes (a
// theme too big for the heap is the usual cause) setup() can tell the theme
// was involved and boot into the built-in clock instead of the same crash.
constexpr uint32_t KEEP_THEME = 1u << 0;
void themeCrashGuard(bool armed);
// Arms the breadcrumb for one scope (a validation in a web handler) and puts
// back whatever it was afterwards.
class ThemeCrashScope {
 public:
  ThemeCrashScope() : was_(platformKeepRead() & KEEP_THEME) { themeCrashGuard(true); }
  ~ThemeCrashScope() { themeCrashGuard(was_); }
 private:
  bool was_;
};
// Called once from setup() when the last reset was a crash, with the
// breadcrumbs it left behind. If a theme was involved, turns the active theme
// off (falling back to the built-in clock, saved to settings) and returns true:
// the cause is handled, so the device boots normally instead of in safe mode.
bool themeRecoverAfterCrash(Settings& s, uint32_t keep);

class ThemeMode : public DisplayMode {
 public:
  const char* id() const override { return "theme"; }
  uint8_t modeConst() const override { return MODE_THEME; }
  void begin(const Settings&) override;
  void service(const Settings&) override;
  void invalidate(const Settings&) override;
  void wake(const Settings&) override {
    engine_.invalidate();
    messageDrawn_ = false;
  }
  const String& error() const { return error_; }
  // Set when this boot recovered from a theme crash; `recoveredId()` is the
  // theme that was turned off (empty if the crash came from a validation).
  bool recovered() const { return recovered_; }
  const String& recoveredId() const { return recoveredId_; }
  void setRecovered(const String& id) {
    recovered_ = true;
    recoveredId_ = id;
  }
  void clearRecovered() {
    recovered_ = false;
    recoveredId_ = "";
    messageDrawn_ = false;
  }
 private:
  void unload();
  void applyLocation(const Settings&);
  void refreshData();
  void renderBuiltin();
  smalltv::Engine engine_;
  std::unique_ptr<ThemeFile> file_;
  std::unique_ptr<smalltv::Package> package_;
  String loadedId_, error_;
  std::vector<uint32_t> dataNextMs_;
  std::vector<bool> dataScheduled_;
  std::vector<smalltv::ThemeValue> dataValues_;
  smalltv::DataRequests dataRequests_;
  size_t dataCursor_ = 0;
  smalltv::Location location_;
  String city_;
  bool attempted_ = false, messageDrawn_ = false, locationApplied_ = false;
  String recoveredId_, builtinIp_;
  int builtinMinute_ = -2;
  bool recovered_ = false;
};
extern ThemeMode g_themeMode;
#endif
