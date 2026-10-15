// CompanionWeb.cpp — the companion app's HTTP API (features/companion).
//
//   POST /api/companion/rect?x=&y=&w=&h=   multipart, one file: w*h RGB565 LE
//   POST /api/companion/glyphs             multipart, one file: "SCG1" atlas
//   POST /api/companion/clock              JSON {enabled,x,y,w,h,bg,fg}
//   GET  /api/companion/status[?hb=1]      state + epoch; hb=1 is a heartbeat
//
// ESP8266WebServer buffers a plain request body whole in RAM, but streams a
// multipart file through the upload callback in HTTP_UPLOAD_BUFLEN chunks, so
// a full 115 KB frame goes to the panel a few rows at a time and never exists
// in memory. Every reply is {"ok":bool,"epoch":n[,"error":...]}: the epoch
// tells the companion whether the device painted over its frame since.
#include "CompanionWeb.h"
#if WITH_COMPANION
#include <Arduino_GFX_Library.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <memory>
#include "Gfx.h"
#include "CompanionMode.h"
#include "CompanionProto.h"

namespace {
WebServerClass* server;
Settings* settings;
bool (*auth)();

// One upload at a time (the server is single-threaded); shared by both routes.
String uploadError;
bool uploadAllowed = false;
unsigned uploadParts = 0;

// /rect
constexpr size_t RectBufPixels = 1024;   // 2 KB, allocated only while a rect streams
std::unique_ptr<uint16_t[]> rectBuf;
companion::PixelStream pixels;
int rectX = 0, rectY = 0, rectW = 0, rectH = 0;

// /glyphs
const char* staging = "/companion/.upload";
const char* atlasPath = "/companion/glyphs.bin";
File atlasUpload;

void reply(int code, const char* error = nullptr) {
  JsonDocument doc;
  doc["ok"] = code < 300;
  doc["epoch"] = g_companionMode.epoch();
  if (error) doc["error"] = error;
  String json;
  serializeJson(doc, json);
  server->send(code, "application/json", json);
}

// Same rule as ThemeWeb: the upload callback runs before the route handler,
// so auth is decided on the first chunk and the body is drained unread when
// it fails; the route handler then asks again to send the challenge.
bool uploadAuthorized() {
  return !settings->auth.enabled || !settings->auth.pass.length() ||
         server->authenticate(settings->auth.user.c_str(), settings->auth.pass.c_str());
}

bool startUpload() {
  if (++uploadParts > 1) {
    uploadError = "upload exactly one file";
    return false;
  }
  uploadError = "";
  uploadAllowed = uploadAuthorized();
  return uploadAllowed;
}

void endRect() { rectBuf.reset(); }

void drawRows(int row, const uint16_t* px, int rows) {
  // The buffer is ours: tint in place rather than copying.
  uint16_t* p = const_cast<uint16_t*>(px);
  for (int i = 0, n = rows * rectW; i < n; ++i) p[i] = gfxTint(p[i]);
  gfxDev()->draw16bitRGBBitmap(rectX, rectY + row, p, rectW, rows);
  yield();
}

void receiveRect() {
  HTTPUpload& up = server->upload();
  if (up.status == UPLOAD_FILE_START) {
    endRect();
    if (!startUpload()) return;
    long x = server->arg("x").toInt(), y = server->arg("y").toInt();
    long w = server->arg("w").toInt(), h = server->arg("h").toInt();
    if (!companion::validRect(x, y, w, h)) { uploadError = "bad rect"; return; }
    if (!gfxDev()) { uploadError = "no display"; return; }
    if (!g_companionMode.onScreen()) { uploadError = "not_active"; return; }
    rectX = x; rectY = y; rectW = w; rectH = h;
    rectBuf.reset(new (std::nothrow) uint16_t[RectBufPixels]);
    if (!rectBuf) { uploadError = "out of memory"; return; }
    pixels.begin(w, h, rectBuf.get(), RectBufPixels);
    g_companionMode.touch();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadAllowed || uploadError.length() || !rectBuf) return;
    if (!pixels.feed(up.buf, up.currentSize, drawRows)) {
      uploadError = "more pixels than w*h";
      endRect();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!uploadAllowed || uploadError.length() || !rectBuf) return;
    pixels.finish(drawRows);
    if (!pixels.complete()) uploadError = "fewer pixels than w*h";
    endRect();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    endRect();
    uploadError = "upload aborted";
  }
}

void finishRect() {
  bool allowed = uploadAllowed;
  uploadParts = 0;
  uploadAllowed = false;
  endRect();
  if (!auth()) return;
  if (!allowed && !uploadError.length()) uploadError = "no file uploaded";
  if (uploadError == "not_active") { reply(409, "not_active"); return; }
  if (uploadError.length()) { reply(400, uploadError.c_str()); return; }
  g_companionMode.rectDrawn(rectX, rectY, rectW, rectH);
  reply(200);
}

void receiveGlyphs() {
  HTTPUpload& up = server->upload();
  if (up.status == UPLOAD_FILE_START) {
    atlasUpload.close();
    if (!startUpload()) return;
    LittleFS.remove(staging);
    atlasUpload = LittleFS.open(staging, "w");
    if (!atlasUpload) uploadError = "cannot create staging file";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadAllowed || uploadError.length()) return;
    if (atlasUpload.size() + up.currentSize > 64 * 1024) {
      uploadError = "atlas larger than 64 KB";
      atlasUpload.close();
    } else if (atlasUpload.write(up.buf, up.currentSize) != up.currentSize) {
      uploadError = "not enough storage";
      atlasUpload.close();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    atlasUpload.close();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    atlasUpload.close();
    uploadError = "upload aborted";
  }
}

void finishGlyphs() {
  bool allowed = uploadAllowed;
  uploadParts = 0;
  uploadAllowed = false;
  atlasUpload.close();
  if (!auth()) { LittleFS.remove(staging); return; }
  if (!allowed && !uploadError.length()) uploadError = "no file uploaded";
  if (!uploadError.length()) {
    File f = LittleFS.open(staging, "r");
    uint8_t hdr[companion::AtlasHeaderSize];
    companion::Atlas a;
    bool ok = f && f.read(hdr, sizeof(hdr)) == sizeof(hdr) &&
              companion::parseAtlas(hdr, sizeof(hdr), f.size(), a);
    if (f) f.close();
    if (!ok) uploadError = "not a valid SCG1 atlas";
  }
  if (uploadError.length()) {
    LittleFS.remove(staging);
    reply(400, uploadError.c_str());
    return;
  }
  LittleFS.remove(atlasPath);
  if (!LittleFS.rename(staging, atlasPath)) { reply(500, "cannot store atlas"); return; }
  g_companionMode.touch();
  g_companionMode.atlasChanged();
  reply(200);
}

void postClock() {
  if (!auth()) return;
  JsonDocument doc;
  if (!server->hasArg("plain") || deserializeJson(doc, server->arg("plain"))) {
    reply(400, "expected a JSON body");
    return;
  }
  CompanionClock c;
  c.enabled = doc["enabled"] | true;
  c.x = doc["x"] | 0; c.y = doc["y"] | 0; c.w = doc["w"] | 0; c.h = doc["h"] | 0;
  c.bg = doc["bg"] | 0;
  c.fg = doc["fg"] | 0xFFFF;
  if (c.enabled && !companion::validRect(c.x, c.y, c.w, c.h)) { reply(400, "bad rect"); return; }
  g_companionMode.touch();
  if (!g_companionMode.setClock(c)) { reply(500, "cannot store clock"); return; }
  reply(200);
}

void getStatus() {
  if (!auth()) return;
  if (server->arg("hb") == "1") g_companionMode.touch();
  JsonDocument doc;
  doc["ok"] = true;
  doc["epoch"] = g_companionMode.epoch();
  doc["active"] = g_companionMode.onScreen();
  doc["live"] = g_companionMode.live();
  uint32_t ago = g_companionMode.lastSeenAgoMs();
  if (ago != 0xFFFFFFFFu) doc["lastSeenAgoS"] = ago / 1000;
  doc["offlineSec"] = settings->companion.offlineSec;
  doc["w"] = companion::Panel;
  doc["h"] = companion::Panel;
  doc["glyphs"] = g_companionMode.hasAtlas();
  JsonObject c = doc["clock"].to<JsonObject>();
  const CompanionClock& k = g_companionMode.clock();
  c["enabled"] = k.enabled;
  c["x"] = k.x; c["y"] = k.y; c["w"] = k.w; c["h"] = k.h;
  c["bg"] = k.bg; c["fg"] = k.fg;
  doc["heap"] = ESP.getFreeHeap();
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}
}  // namespace

void companionWebBegin(WebServerClass& web, Settings& s, bool (*requireAuth)()) {
  server = &web;
  settings = &s;
  auth = requireAuth;
  web.on("/api/companion/rect", HTTP_POST, finishRect, receiveRect);
  web.on("/api/companion/glyphs", HTTP_POST, finishGlyphs, receiveGlyphs);
  web.on("/api/companion/clock", HTTP_POST, postClock);
  web.on("/api/companion/status", HTTP_GET, getStatus);
}
#endif  // WITH_COMPANION
