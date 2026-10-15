// CompanionMode.h — screens pushed by the PC companion app (features/companion).
//
// The companion (companion/ in this repo) composes the whole 240x240 screen on
// the PC and pushes only the rectangles that changed (CompanionWeb.cpp); this
// mode just keeps the panel honest around those pushes:
//   - it draws the clock itself, once a minute, in the box the companion
//     reserved for it, so an idle PC has nothing to send;
//   - it shows a waiting screen until the first frame lands and an offline
//     screen when the companion goes quiet for s.companion.offlineSec;
//   - every time the device paints over companion content (boot, wake,
//     offline) it bumps epoch(), which the companion polls to know it has to
//     resend a full frame.
#pragma once
#include "Mode.h"
#include "config.h"

struct CompanionClock {
  bool     enabled = false;
  int16_t  x = 0, y = 0, w = 0, h = 0;
  uint16_t bg = 0x0000;     // RGB565, fills the box before the digits
  uint16_t fg = 0xFFFF;     // RGB565, only for the built-in font fallback
};

class CompanionMode : public DisplayMode {
 public:
  const char* id() const override { return "companion"; }
  uint8_t     modeConst() const override { return MODE_COMPANION; }

  void begin(const Settings& s) override;
  void service(const Settings& s) override;
  void invalidate(const Settings& s) override { repaintIdle(); }
  void wake(const Settings& s) override { repaintIdle(); }

  // --- called by CompanionWeb ---
  bool onScreen() const;                 // serviced within the last second
  void touch() { lastSeen_ = millis(); } // any companion request is a heartbeat
  void rectDrawn(int x, int y, int w, int h);
  bool setClock(const CompanionClock& c);
  void atlasChanged();                   // /companion/glyphs.bin replaced or removed

  const CompanionClock& clock() const { return clock_; }
  bool     hasAtlas() const { return atlasOk_; }
  bool     live() const { return state_ == LIVE; }
  uint32_t epoch() const { return epoch_; }
  uint32_t lastSeenAgoMs() const { return lastSeen_ ? millis() - lastSeen_ : 0xFFFFFFFFu; }

 private:
  enum State : uint8_t { WAITING, LIVE, OFFLINE };
  void repaintIdle();
  void drawIdle(bool full);
  void drawClock();

  State    state_ = WAITING;
  bool     idleDrawn_ = false;
  bool     clockDirty_ = false;
  bool     atlasOk_ = false;
  int      minute_ = -2;
  uint32_t epoch_ = 1;
  uint32_t lastSeen_ = 0, lastService_ = 0;
  uint16_t offlineSec_ = COMPANION_OFFLINE_SEC_DEFAULT;
  String   ip_;
  CompanionClock clock_;
};

extern CompanionMode g_companionMode;
