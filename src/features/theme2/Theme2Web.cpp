// Theme2Web.cpp — HTTP API for theme v2. The browser editor compiles the
// theme and POSTs the STV2 file here; the device validates it with the same
// reader the renderer uses before it replaces the active theme.
//
//   POST /api/theme2            multipart `theme` (.stv): install + activate
//   GET  /api/theme2/info       format version, active theme, free space
//   POST /api/theme2/select     show the installed theme
//   POST /api/theme2/rollback   swap the active and the previous theme
//   GET  /api/theme2/source     the editor's source JSON (opaque to the device)
//   POST /api/theme2/source     multipart `source`: store it
//   GET  /api/theme2/snapshot   ?screen=N: the screen as raw RGB565, rendered
//                               by the same code that drives the panel
#include "Theme2Web.h"
#if WITH_THEME2
#include "Theme2Mode.h"
#include "Clock.h"
#include "features/theme/ThemeMode.h"
#include <ArduinoJson.h>

extern void appInvalidate();
namespace {
WebServerClass* server;
Settings* settings;
bool (*auth)();
File upload;
String uploadError;
const char* uploadTarget = nullptr;
uint32_t uploaded = 0, budget = 0;
bool uploadAllowed = false;
constexpr uint32_t MaxSource = 256 * 1024;

void reply(int code, const char* key, const String& message) {
  JsonDocument doc;
  doc[key] = message;
  String json;
  serializeJson(doc, json);
  server->send(code, "application/json", json);
}
size_t freeBytes() {
#if defined(SMALLTV_ESP8266)
  FSInfo info;
  return LittleFS.info(info) ? info.totalBytes - info.usedBytes : 0;
#else
  return LittleFS.totalBytes() - LittleFS.usedBytes();
#endif
}
uint32_t fileSize(const char* path) {
  File f = LittleFS.open(path, "r");
  return f ? f.size() : 0;
}

// Streams one multipart file to the staging path, refusing early when it
// cannot fit (keeping 16 KB for settings and filesystem overhead).
void receive(const char* target, uint32_t limit) {
  HTTPUpload& up = server->upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadError = "";
    uploaded = 0;
    uploadTarget = target;
    uploadAllowed = !settings->auth.enabled || !settings->auth.pass.length() ||
                    server->authenticate(settings->auth.user.c_str(), settings->auth.pass.c_str());
    if (!uploadAllowed) return;
    upload.close();
    LittleFS.remove(theme2::Staging);
    // The file being replaced frees its space once the upload is renamed over it.
    size_t free = freeBytes() + (target == theme2::SourceJson ? fileSize(theme2::SourceJson) : fileSize(theme2::Previous));
    if (free <= 16384) { uploadError = "Not enough storage"; return; }
    budget = std::min<uint32_t>(limit, free - 16384);
    upload = LittleFS.open(theme2::Staging, "w");
    if (!upload) uploadError = "Cannot create staging file";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (!uploadAllowed || uploadError.length()) return;
    if (up.currentSize > budget - uploaded) {
      uploadError = "File too large for the available storage";
      upload.close();
      return;
    }
    if (upload.write(up.buf, up.currentSize) != up.currentSize) {
      uploadError = "Write failed";
      upload.close();
      return;
    }
    uploaded += up.currentSize;
  } else if (up.status == UPLOAD_FILE_END) {
    upload.close();
    if (!uploadError.length() && !uploaded) uploadError = "Empty upload";
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    upload.close();
    uploadError = "Upload aborted";
  }
}

bool uploadOk() {
  upload.close();
  if (!auth()) return false;
  if (!uploadAllowed || uploadError.length() || !uploaded) {
    reply(400, "error", uploadError.length() ? uploadError : String("No file uploaded"));
    LittleFS.remove(theme2::Staging);
    return false;
  }
  return true;
}

// Selects the editor theme as the active mode and persists it.
bool activate() {
  uint8_t old = settings->mode;
  settings->mode = MODE_THEME2;
  if (!saveSettings(*settings)) {
    settings->mode = old;
    return false;
  }
  g_themeMode.clearRecovered();
  g_theme2Mode.reload();
  clockReapply(*settings);
  appInvalidate();
  return true;
}

void finishTheme() {
  if (!uploadOk()) return;
  {
    // Validate exactly as the renderer will open it; crash-guarded like any
    // theme work, so a pathological file cannot boot-loop the device.
    ThemeCrashScope guard;
    Theme2File file;
    stv2::Theme theme;
    char err[64] = "";
    bool valid = file.open(theme2::Staging) && theme.open(file, err, sizeof err);
    file.close();
    if (!valid) {
      LittleFS.remove(theme2::Staging);
      reply(400, "error", String("Invalid theme: ") + (err[0] ? err : "unreadable"));
      return;
    }
  }
  g_theme2Mode.reload();   // close the active file before renaming it
  LittleFS.remove(theme2::Previous);
  if (LittleFS.exists(theme2::Active)) LittleFS.rename(theme2::Active, theme2::Previous);
  if (!LittleFS.rename(theme2::Staging, theme2::Active)) {
    reply(500, "error", "Could not install the theme");
    return;
  }
  if (!activate()) {
    reply(500, "error", "Installed, but could not save the selection");
    return;
  }
  reply(200, "ok", String(uploaded));
}

void finishSource() {
  if (!uploadOk()) return;
  LittleFS.remove(theme2::SourceJson);
  if (!LittleFS.rename(theme2::Staging, theme2::SourceJson)) {
    reply(500, "error", "Could not store the source");
    return;
  }
  reply(200, "ok", String(uploaded));
}

void getSource() {
  if (!auth()) return;
  File f = LittleFS.open(theme2::SourceJson, "r");
  if (!f) {
    reply(404, "error", "No source stored");
    return;
  }
  server->streamFile(f, "application/json");
  f.close();
}

void info() {
  if (!auth()) return;
  JsonDocument doc;
  doc["format"] = stv2::FormatVersion;
  doc["active"] = settings->mode == MODE_THEME2;
  doc["installed"] = LittleFS.exists(theme2::Active);
  doc["bytes"] = fileSize(theme2::Active);
  doc["previous"] = LittleFS.exists(theme2::Previous);
  doc["source"] = LittleFS.exists(theme2::SourceJson);
  doc["name"] = g_theme2Mode.name();
  doc["error"] = g_theme2Mode.error();
  doc["screens"] = g_theme2Mode.screenCount();
  doc["screen"] = g_theme2Mode.currentScreen();
  doc["freeBytes"] = freeBytes();
  doc["heap"] = ESP.getFreeHeap();
  if (g_themeMode.recovered()) doc["recovered"] = true;
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void select() {
  if (!auth()) return;
  if (!LittleFS.exists(theme2::Active)) {
    reply(404, "error", "No theme installed");
    return;
  }
  if (!activate()) {
    reply(500, "error", "Could not save the selection");
    return;
  }
  reply(200, "ok", "selected");
}

void rollback() {
  if (!auth()) return;
  if (!LittleFS.exists(theme2::Previous)) {
    reply(404, "error", "No previous theme");
    return;
  }
  g_theme2Mode.reload();
  LittleFS.remove(theme2::Staging);
  bool ok = LittleFS.rename(theme2::Active, theme2::Staging) && LittleFS.rename(theme2::Previous, theme2::Active) &&
            LittleFS.rename(theme2::Staging, theme2::Previous);
  if (!ok || !activate()) {
    reply(500, "error", "Could not restore the previous theme");
    return;
  }
  reply(200, "ok", "restored");
}

void sendBand(const uint16_t* band, int, int rows, void*) {
  server->sendContent(reinterpret_cast<const char*>(band), size_t(stv2::Width) * rows * 2);
}
void snapshot() {
  if (!auth()) return;
  uint16_t index = server->hasArg("screen") ? uint16_t(server->arg("screen").toInt()) : g_theme2Mode.currentScreen();
  if (!g_theme2Mode.loaded() || index >= g_theme2Mode.screenCount()) {
    reply(409, "error", "No editor theme is running, or no such screen");
    return;
  }
  server->setContentLength(size_t(stv2::Width) * stv2::Height * 2);
  server->send(200, "application/octet-stream", "");
  g_theme2Mode.snapshot(index, sendBand, nullptr);
}
}  // namespace

void theme2WebBegin(WebServerClass& web, Settings& s, bool (*requireAuth)()) {
  server = &web;
  settings = &s;
  auth = requireAuth;
  web.on("/api/theme2", HTTP_POST, finishTheme, [] { receive(theme2::Active, stv2::MaxFile); });
  web.on("/api/theme2/source", HTTP_POST, finishSource, [] { receive(theme2::SourceJson, MaxSource); });
  web.on("/api/theme2/source", HTTP_GET, getSource);
  web.on("/api/theme2/info", HTTP_GET, info);
  web.on("/api/theme2/select", HTTP_POST, select);
  web.on("/api/theme2/rollback", HTTP_POST, rollback);
  web.on("/api/theme2/snapshot", HTTP_GET, snapshot);
}
#endif
