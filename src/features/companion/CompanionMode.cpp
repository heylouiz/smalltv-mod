// CompanionMode.cpp — see CompanionMode.h.
#include "CompanionMode.h"
#if WITH_COMPANION
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "Clock.h"
#include "Gfx.h"
#include "Net.h"
#include "CompanionProto.h"

CompanionMode g_companionMode;

static const char* kClockPath = "/companion/clock.json";
static const char* kAtlasPath = "/companion/glyphs.bin";

static bool loadAtlas(companion::Atlas& a) {
  File f = LittleFS.open(kAtlasPath, "r");
  if (!f) return false;
  uint8_t hdr[companion::AtlasHeaderSize];
  bool ok = f.read(hdr, sizeof(hdr)) == sizeof(hdr) &&
            companion::parseAtlas(hdr, sizeof(hdr), f.size(), a);
  f.close();
  return ok;
}

void CompanionMode::begin(const Settings& s) {
  offlineSec_ = s.companion.offlineSec;
  LittleFS.mkdir("/companion");
  LittleFS.remove("/companion/.upload");   // an interrupted atlas upload
  File f = LittleFS.open(kClockPath, "r");
  if (f) {
    JsonDocument doc;
    if (!deserializeJson(doc, f)) {
      clock_.enabled = doc["enabled"] | false;
      clock_.x = doc["x"] | 0;
      clock_.y = doc["y"] | 0;
      clock_.w = doc["w"] | 0;
      clock_.h = doc["h"] | 0;
      clock_.bg = doc["bg"] | 0;
      clock_.fg = doc["fg"] | 0xFFFF;
    }
    f.close();
  }
  atlasChanged();
  repaintIdle();
}

bool CompanionMode::onScreen() const {
  return lastService_ && millis() - lastService_ < 1000;
}

void CompanionMode::repaintIdle() {
  state_ = lastSeen_ && millis() - lastSeen_ < (uint32_t)offlineSec_ * 1000UL ? WAITING : OFFLINE;
  if (!lastSeen_) state_ = WAITING;
  idleDrawn_ = false;
  ++epoch_;
}

void CompanionMode::rectDrawn(int x, int y, int w, int h) {
  touch();
  if (state_ != LIVE) {
    state_ = LIVE;
    minute_ = -2;          // the frame replaced the idle screen: draw our clock
  }
  const CompanionClock& c = clock_;
  if (c.enabled && x < c.x + c.w && c.x < x + w && y < c.y + c.h && c.y < y + h)
    clockDirty_ = true;    // the companion painted over the clock box
}

bool CompanionMode::setClock(const CompanionClock& c) {
  clock_ = c;
  clockDirty_ = true;
  JsonDocument doc;
  doc["enabled"] = c.enabled;
  doc["x"] = c.x; doc["y"] = c.y; doc["w"] = c.w; doc["h"] = c.h;
  doc["bg"] = c.bg; doc["fg"] = c.fg;
  File f = LittleFS.open(kClockPath, "w");
  if (!f) return false;
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}

void CompanionMode::atlasChanged() {
  companion::Atlas a;
  atlasOk_ = loadAtlas(a);
  clockDirty_ = true;
}

// Waiting / offline screen: the device's own clock and IP, in the same layout
// as the theme mode's built-in clock, plus what it is waiting for.
void CompanionMode::drawIdle(bool full) {
  Arduino_GFX* gfx = gfxDev();
  if (!gfx) return;
  if (full) {
    gfx->fillScreen(C_BLACK);
    if (state_ == OFFLINE) {
      gfxDrawCentered("COMPANION OFFLINE", 12, 2, C_YELLOW);
      gfxDrawCentered("no update from the PC app", 34, 1, C_GRAY);
    } else {
      gfxDrawCentered("COMPANION", 12, 2, C_WHITE);
      gfxDrawCentered("waiting for the PC app", 34, 1, C_GRAY);
    }
    gfxDrawCentered("smalltv-companion run", 214, 1, C_DGRAY);
    minute_ = -2;
    ip_ = "";
  }
  tm t{};
  int minute = clockNow(t) ? t.tm_hour * 60 + t.tm_min : -1;
  if (minute != minute_) {
    minute_ = minute;
    char hhmm[6];
    if (minute < 0) strcpy(hhmm, "--:--");
    else snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
    gfx->setTextSize(7);
    gfx->setTextColor(C_WHITE, C_BLACK);
    gfx->setCursor((TFT_WIDTH - gfxTextW(hhmm, 7)) / 2, 84);
    gfx->print(hhmm);
  }
  String ip = netIP();
  if (ip != ip_) {
    ip_ = ip;
    gfx->fillRect(0, 170, TFT_WIDTH, 24, C_BLACK);
    const char* shown = ip.length() ? ip.c_str() : "-";
    gfxDrawCentered(shown, 170, gfxFitSize(shown, 232, 3), C_GREEN);
  }
}

// The companion's clock box: background fill, then HH:MM centred, from the
// pushed digit atlas when there is one (copied row by row out of LittleFS)
// and from the built-in font otherwise.
void CompanionMode::drawClock() {
  Arduino_GFX* gfx = gfxDev();
  const CompanionClock& c = clock_;
  if (!gfx || !c.enabled || c.w <= 0 || c.h <= 0) return;
  tm t{};
  bool synced = clockNow(t);
  char hhmm[6];
  if (synced) snprintf(hhmm, sizeof(hhmm), "%02d:%02d", t.tm_hour, t.tm_min);
  else strcpy(hhmm, "--:--");

  gfx->fillRect(c.x, c.y, c.w, c.h, gfxTint(c.bg));

  companion::Atlas a;
  if (synced && atlasOk_ && loadAtlas(a)) {
    int total = 0;
    for (const char* p = hhmm; *p; ++p) total += a.widths[companion::atlasIndex(*p)];
    if (total <= c.w && a.height <= c.h) {
      File f = LittleFS.open(kAtlasPath, "r");
      if (f) {
        int x = c.x + (c.w - total) / 2, y = c.y + (c.h - a.height) / 2;
        uint16_t line[companion::Panel];
        for (const char* p = hhmm; *p; ++p) {
          int i = companion::atlasIndex(*p), w = a.widths[i];
          f.seek(a.offsets[i]);
          for (int r = 0; r < a.height; ++r) {
            if (f.read((uint8_t*)line, w * 2) != (size_t)w * 2) break;
            for (int k = 0; k < w; ++k) line[k] = gfxTint(line[k]);
            gfx->draw16bitRGBBitmap(x, y + r, line, w, 1);
          }
          x += w;
        }
        f.close();
        return;
      }
    }
  }
  uint8_t size = gfxFitSize(hhmm, c.w - 4, (uint8_t)constrain(c.h / 8, 1, 8));
  gfx->setTextSize(size);
  gfx->setTextColor(gfxTint(c.fg), gfxTint(c.bg));
  gfx->setCursor(c.x + (c.w - gfxTextW(hhmm, size)) / 2, c.y + (c.h - 8 * size) / 2);
  gfx->print(hhmm);
}

void CompanionMode::service(const Settings& s) {
  lastService_ = millis();
  offlineSec_ = s.companion.offlineSec;

  if (state_ == LIVE && millis() - lastSeen_ >= (uint32_t)offlineSec_ * 1000UL) {
    state_ = OFFLINE;
    idleDrawn_ = false;
    ++epoch_;
  }

  if (state_ != LIVE) {
    drawIdle(!idleDrawn_);
    idleDrawn_ = true;
    return;
  }

  tm t{};
  int minute = clockNow(t) ? t.tm_hour * 60 + t.tm_min : -1;
  if (minute != minute_) {
    minute_ = minute;
    clockDirty_ = true;
  }
  if (clockDirty_) {
    clockDirty_ = false;
    drawClock();
  }
}

#endif  // WITH_COMPANION
