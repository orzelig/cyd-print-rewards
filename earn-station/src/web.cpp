// Small web server on the station: status at /, and firmware updates at /update.
// An update must carry the station's pairing key (header X-Earn-Key), which only the
// print quota device (and its admin) knows.
#include "station.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_app_desc.h>

static WebServer server(80);
static bool started = false, updAllowed = false;
static String updError;

static bool keyOk() {
  if (bankKeyMatches(server.header("X-Earn-Key"))) return true;
  server.send(401, "text/plain", "Not allowed");
  return false;
}

static void handleStatus() {
  JsonDocument d;
  d["device"] = "earning station";
  d["version"] = FW_VERSION;
  d["paired"] = bankPaired();
  d["queued"] = queuedCount();
  d["reading"] = sessions.size();
  char build[12];
  esp_app_get_elf_sha256(build, sizeof build);  // fingerprint of this exact build
  d["build"] = build;
  d["uptime_s"] = millis() / 1000;
  d["reset_reason"] = (int)esp_reset_reason();
  d["min_free_heap"] = ESP.getMinFreeHeap();
  d["time"] = (uint32_t)time(nullptr);
  amazonStatus(d["kindle"].to<JsonObject>());  // names and minutes only
  String s;
  serializeJson(d, s);
  server.send(200, "application/json", s);
}

static void handleUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    updError = "";
    updAllowed = bankKeyMatches(server.header("X-Earn-Key"));
    if (!updAllowed) return;
    Serial.printf("firmware update: %s\n", up.filename.c_str());
    // Save any reading in progress so it's sent after the restart
    while (!sessions.empty()) stopReading(sessions.front().userId, true);
    uiMessage("Updating...", "Don't unplug the station");
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) updError = Update.errorString();
  } else if (!updAllowed || updError.length()) {
    return;
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      updError = Update.errorString();
      Update.abort();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) updError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    updError = "Upload interrupted";
  }
}

static void handleUpdateDone() {
  if (!updAllowed) return server.send(401, "text/plain", "Not allowed");
  if (updError.length() || !Update.isFinished()) {
    String e = updError.length() ? updError : String("No firmware received");
    updError = "";
    uiDirty = true;
    return server.send(400, "text/plain", e);
  }
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.println("firmware updated, restarting");
  delay(500);
  ESP.restart();
}

void webLoop() {
  if (!started && WiFi.status() == WL_CONNECTED && !wifiPortalActive()) {
    static const char* hdrs[] = {"X-Earn-Key"};
    server.collectHeaders(hdrs, 1);
    server.on("/", HTTP_GET, handleStatus);
    server.on("/update", HTTP_POST, handleUpdateDone, handleUpload);
    // Screenshot, remote taps and screen previews, for checking the touchscreen from a computer
    server.on("/screen", HTTP_GET, [] {
      if (!bankKeyMatches(server.header("X-Earn-Key"))) return server.send(401, "text/plain", "Not allowed");
      server.sendHeader("Cache-Control", "no-store");
      server.setContentLength(SCREEN_BMP_SIZE);
      server.send(200, "image/bmp", "");
      uiCapture(server.client());
    });
    // Kindle reading: the sign-in values can be written here but are never sent back
    server.on("/amazon/setup", HTTP_POST, [] {
      if (!keyOk()) return;
      String err;
      if (!amazonSetup(server.arg("plain"), err)) return server.send(400, "text/plain", err);
      server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/amazon/forget", HTTP_POST, [] {
      if (!keyOk()) return;
      amazonForget();
      server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/amazon/fetch", HTTP_POST, [] {
      if (!keyOk()) return;
      amazonFetchNow();
      server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/ui", HTTP_POST, [] {
      if (!bankKeyMatches(server.header("X-Earn-Key"))) return server.send(401, "text/plain", "Not allowed");
      int x, y;
      if (server.hasArg("tap") && sscanf(server.arg("tap").c_str(), "%d,%d", &x, &y) == 2) uiTap(x, y);
      else if (!server.hasArg("preview") || !uiPreview(server.arg("preview"))) return server.send(400, "text/plain", "Bad request");
      server.send(200, "application/json", "{\"ok\":true}");
    });
    server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
    server.begin();
    started = true;
    Serial.printf("station web: http://%s/\n", WiFi.localIP().toString().c_str());
  }
  if (started) server.handleClient();
}
