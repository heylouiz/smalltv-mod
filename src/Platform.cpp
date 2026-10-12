// Platform.cpp — the few things that need a real definition (not header-inline)
// but are still core-specific. Keeps the per-chip SNTP callback wiring out of the
// feature code, same as the inline shims in Platform.h.
#include "Platform.h"

#if defined(SMALLTV_ESP32C2) || defined(SMALLTV_ESP32)
#include <esp_sntp.h>
#include <esp_attr.h>

// .noinit RAM keeps its contents across a panic, a watchdog and esp_restart();
// after a power cut it holds garbage, which the magic word filters out. The
// ESP32 records panics in esp_reset_reason(), so KEEP_CRASHED is never set here.
static constexpr uint32_t kKeepMagic = 0x5354564bu;   // "STVK"
__NOINIT_ATTR static uint32_t s_keep[2];

uint32_t platformKeepRead() { return s_keep[0] == kKeepMagic ? s_keep[1] : 0; }
void platformKeepWrite(uint32_t flags) {
  s_keep[1] = flags;
  s_keep[0] = kKeepMagic;
}

static void (*s_syncCb)() = nullptr;
static void sntpSyncNotify(struct timeval*) { if (s_syncCb) s_syncCb(); }

void platformOnTimeSync(void (*cb)()) {
  s_syncCb = cb;
  sntp_set_time_sync_notification_cb(sntpSyncNotify);
}

#else  // ESP8266
#include <coredecls.h>

// RTC user memory keeps its contents across every reset except a power cut,
// when the magic word no longer matches. eboot owns the first 128 bytes (OTA
// commands), so the breadcrumbs sit in the last two of its 128 4-byte blocks.
static constexpr uint32_t kKeepBlock = 126;
static constexpr uint32_t kKeepMagic = 0x5354564bu;   // "STVK"

uint32_t platformKeepRead() {
  uint32_t keep[2] = {0, 0};
  ESP.rtcUserMemoryRead(kKeepBlock, keep, sizeof(keep));
  return keep[0] == kKeepMagic ? keep[1] : 0;
}
void platformKeepWrite(uint32_t flags) {
  uint32_t keep[2] = {kKeepMagic, flags};
  ESP.rtcUserMemoryWrite(kKeepBlock, keep, sizeof(keep));
}

// The core calls this from its postmortem handler on every crash it reports
// (exception, soft watchdog, panic/abort, failed allocation), right before
// restarting. It is the only trace an out-of-memory abort leaves behind.
extern "C" void custom_crash_callback(struct rst_info*, uint32_t, uint32_t) {
  platformKeepWrite(platformKeepRead() | KEEP_CRASHED);
}

static void (*s_syncCb)() = nullptr;
// settimeofday_cb fires whenever the clock is set; from_sntp distinguishes an
// SNTP update (what we care about) from a manual settimeofday (which we never do).
static void sntpSyncNotify(bool from_sntp) { if (from_sntp && s_syncCb) s_syncCb(); }

void platformOnTimeSync(void (*cb)()) {
  s_syncCb = cb;
  settimeofday_cb(sntpSyncNotify);
}
#endif
