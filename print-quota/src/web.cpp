// Admin web page + JSON API at http://printquota.local
#include "app.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <esp_core_dump.h>
#include <esp_app_desc.h>
#include "web_page.h"
#include "web_util.h"

WebServer server(80);
static bool started = false;

bool guard() {
  if (settings.adminPass.isEmpty() || server.authenticate("admin", settings.adminPass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Print Quota");
  return false;
}

bool readBody(JsonDocument& d) {
  if (deserializeJson(d, server.arg("plain"))) {
    server.send(400, "text/plain", "Bad request");
    return false;
  }
  return true;
}

void sendJson(JsonDocument& d) {
  String s;
  serializeJson(d, s);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
}

void sendOk() { server.send(200, "application/json", "{\"ok\":true}"); }
void fail(int code, const char* msg) { server.send(code, "text/plain", msg); }

static void handleState() {
  if (!guard()) return;
  JsonDocument d;
  JsonObject p = d["printer"].to<JsonObject>();
  p["state"] = pstateName(printer.state);
  p["percent"] = printer.percent;
  p["remaining_min"] = printer.remainingMin;
  p["job"] = printer.job;
  p["layer"] = printer.layer;
  p["total_layers"] = printer.totalLayers;
  p["error"] = printer.error;
  JsonObject j = d["job"].to<JsonObject>();
  j["active"] = job.active;
  j["user"] = job.userId;
  j["charged_s"] = job.chargedS;
  j["name"] = job.name;
  d["claim"] = claimUserId;
  d["demo"] = settings.demo;
  d["time_ok"] = timeValid();
  d["now"] = nowEpoch();
  d["history_seq"] = historySeq;
  JsonArray a = d["users"].to<JsonArray>();
  for (auto& u : users) {
    JsonObject o = a.add<JsonObject>();
    o["id"] = u.id;
    o["name"] = u.name;
    o["quota_s"] = u.quotaS;
    o["period"] = periodName(u.period);
    o["used_s"] = u.usedS;
    o["balance_s"] = u.balanceS;
    o["remaining_s"] = u.quotaS < 0 ? 0 : remainingS(u);
    o["can_start"] = canStart(u);
    o["next_reset"] = nextResetEpoch(u);
    o["total_s"] = u.totalS;
    earnUserJson(o, u);
  }
  d["goal_s"] = earn.goalS;
  d["week_end"] = weekEndEpoch();
  d["pair_code"] = earnPendingPair();
  sendJson(d);
}

static void handleUserSave() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  String name = d["name"] | "";
  name.trim();
  if (name.isEmpty() || name.length() > 20) return fail(400, "Name must be 1-20 characters");
  int32_t quota = d["quota_s"] | 0;
  if (quota < -1) quota = -1;
  Period period = parsePeriod(d["period"] | "none");
  if (period == Period::None && quota > 0) quota = 0;  // an allowance needs a reset period

  User* u = nullptr;
  if (d["id"].is<int>()) {
    u = findUser(d["id"]);
    if (!u) return fail(404, "No such user");
    u->name = name;
    u->quotaS = quota;
    if (u->period != period) {
      u->period = period;
      anchorPeriod(*u);
    }
  } else {
    u = addUser(name, quota, period);
    if (!u) return fail(400, "User limit reached");
  }
  if (d["earns"].is<bool>()) u->earns = d["earns"];
  // "Time left" sets the balance so that quota - used + balance == left
  if (d["left_s"].is<int>() && u->quotaS >= 0) {
    u->usedS = 0;
    u->balanceS = d["left_s"].as<int32_t>() - u->quotaS;
  }
  usersChanged();
  sendOk();
}

static void handleUserDelete() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  if (!deleteUser(d["id"] | -1)) return fail(404, "No such user");
  sendOk();
}

static void handleTopup() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  User* u = findUser(d["id"] | -1);
  if (!u) return fail(404, "No such user");
  int32_t s = d["seconds"] | 0;
  if (s <= 0 || s > 1000 * 3600) return fail(400, "Choose an amount of time to add");
  String note = d["note"] | "";
  note.trim();
  if (note.length() > 60) note = note.substring(0, 60);
  u->balanceS += s;
  logTopup(*u, s, note);
  usersChanged();
  sendOk();
}

static void handleReset() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  int id = d["id"] | 0;
  for (auto& u : users)
    if (id == 0 || u.id == id) u.usedS = 0;
  usersChanged();
  sendOk();
}

static void handleAssign() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  if (!job.active) return fail(409, "No print is running");
  assignJob(d["id"] | -1);
  sendOk();
}

static void handleHistory() {
  if (!guard()) return;
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);  // stream it line by line (oldest first)
  server.send(200, "application/json", "");
  server.sendContent("[");
  bool first = true;
  forEachHistoryLine(100, [&](const String& l) {
    if (!first) server.sendContent(",");
    first = false;
    server.sendContent(l);
  });
  server.sendContent("]");
  server.sendContent("");  // end of the chunked reply
}

static void handleSettingsGet() {
  if (!guard()) return;
  JsonDocument d;
  d["printer_ip"] = settings.printerIp;
  d["serial"] = settings.serial;
  d["access_code_set"] = settings.accessCode.length() > 0;
  d["demo"] = settings.demo;
  d["grace_min"] = settings.graceMin;
  d["refund_failed"] = settings.refundFailed;
  d["tz"] = settings.tz;
  d["admin_pass_set"] = settings.adminPass.length() > 0;
  d["version"] = FW_VERSION;
  char build[12];
  esp_app_get_elf_sha256(build, sizeof build);  // fingerprint of this exact build
  d["build"] = build;
  d["uptime_s"] = millis() / 1000;
  d["reset_reason"] = (int)esp_reset_reason();  // 1 power-on, 3 restart, 4 crash, 5/6/7 watchdog, 9 power dip
  d["free_heap"] = ESP.getFreeHeap();
  d["min_free_heap"] = ESP.getMinFreeHeap();
  d["largest_block"] = ESP.getMaxAllocHeap();
  d["low_at"] = heapLowAt;
  d["mqtt_max_len"] = mqttMaxLen;
  sendJson(d);
}

// Last crash saved in flash (the "coredump" partition): task, program counter and backtrace.
// Decode with: xtensa-esp32-elf-addr2line -pfiaC -e firmware.elf <addresses>
static void handleCrash() {
  if (!guard()) return;
  JsonDocument d;
  static esp_core_dump_summary_t s;
  if (esp_core_dump_image_check() != ESP_OK || esp_core_dump_get_summary(&s) != ESP_OK) {
    d["crash"] = false;
    return sendJson(d);
  }
  char hex[12];
  d["crash"] = true;
  d["task"] = s.exc_task;
  snprintf(hex, sizeof hex, "0x%08x", (unsigned)s.exc_pc);
  d["pc"] = hex;
  d["cause"] = s.ex_info.exc_cause;
  snprintf(hex, sizeof hex, "0x%08x", (unsigned)s.ex_info.exc_vaddr);
  d["vaddr"] = hex;
  d["elf_sha"] = (const char*)s.app_elf_sha256;
  d["bt_corrupted"] = s.exc_bt_info.corrupted;
  JsonArray bt = d["backtrace"].to<JsonArray>();
  for (uint32_t i = 0; i < s.exc_bt_info.depth && i < 16; i++) {
    snprintf(hex, sizeof hex, "0x%08x", (unsigned)s.exc_bt_info.bt[i]);
    bt.add(hex);
  }
  sendJson(d);
}

static void handleSettingsPost() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  if (d["printer_ip"].is<const char*>()) {
    settings.printerIp = d["printer_ip"].as<String>();
    settings.printerIp.trim();
  }
  if (d["serial"].is<const char*>()) {
    settings.serial = d["serial"].as<String>();
    settings.serial.trim();
    settings.serial.toUpperCase();
  }
  if (d["access_code"].is<const char*>() && strlen(d["access_code"])) {
    settings.accessCode = d["access_code"].as<String>();
    settings.accessCode.trim();
  }
  if (d["demo"].is<bool>()) settings.demo = d["demo"];
  if (d["grace_min"].is<int>()) settings.graceMin = constrain(d["grace_min"].as<int>(), 0, 240);
  if (d["refund_failed"].is<bool>()) settings.refundFailed = d["refund_failed"];
  if (d["tz"].is<const char*>() && strlen(d["tz"])) settings.tz = d["tz"].as<String>();
  if (d["admin_pass"].is<const char*>() && strlen(d["admin_pass"])) settings.adminPass = d["admin_pass"].as<String>();
  if (d["clear_admin_pass"] | false) settings.adminPass = "";
  saveSettings();
  applySettings();
  uiDirty = true;
  sendOk();
}

static void handleDemo() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  if (!settings.demo) return fail(409, "Turn on demo mode in Settings first");
  String action = d["action"] | "";
  if (action == "start") {
    if (job.active) return fail(409, "A print is already running");
    demoStart(d["minutes"] | 30, d["speed"] | 1);
  } else if (action == "pause") {
    demoTogglePause();
  } else if (action == "finish") {
    demoEnd(true);
  } else if (action == "fail") {
    demoEnd(false);
  } else {
    return fail(400, "Unknown action");
  }
  sendOk();
}

// ---- Firmware update over Wi-Fi (POST /update, multipart file) ----------------------

static bool updAllowed = false;
static String updError;

static void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    updError = "";
    updAllowed = settings.adminPass.isEmpty() || server.authenticate("admin", settings.adminPass.c_str());
    if (!updAllowed) return;
    Serial.printf("firmware update: %s\n", up.filename.c_str());
    saveUsers();  // also saves the running print, which continues after the restart
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
  if (!guard()) return;
  if (updError.length() || !Update.isFinished()) {
    String e = updError.length() ? updError : String("No firmware received");
    updError = "";
    return fail(400, e.c_str());
  }
  server.send(200, "application/json", "{\"ok\":true}");
  Serial.println("firmware updated, restarting");
  delay(500);
  ESP.restart();
}

static void setupRoutes() {
  server.on("/", HTTP_GET, [] {
    if (!guard()) return;
    server.send_P(200, "text/html", INDEX_HTML);
  });
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/api/user", HTTP_POST, handleUserSave);
  server.on("/api/user/delete", HTTP_POST, handleUserDelete);
  server.on("/api/user/topup", HTTP_POST, handleTopup);
  server.on("/api/user/reset", HTTP_POST, handleReset);
  server.on("/api/job/assign", HTTP_POST, handleAssign);
  server.on("/api/history", HTTP_GET, handleHistory);
  server.on("/api/history/clear", HTTP_POST, [] {
    if (!guard()) return;
    clearHistory();
    sendOk();
  });
  server.on("/api/crash", HTTP_GET, handleCrash);
  // Screenshot and remote taps, for checking the touchscreen from a computer
  server.on("/api/screen", HTTP_GET, [] {
    if (!guard()) return;
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(SCREEN_BMP_SIZE);
    server.send(200, "image/bmp", "");
    uiCapture(server.client());
  });
  server.on("/api/ui", HTTP_POST, [] {
    if (!guard()) return;
    JsonDocument d;
    if (!readBody(d)) return;
    if (d["tap"].is<JsonArray>()) uiTap(d["tap"][0] | 0, d["tap"][1] | 0);
    else if (d["preview"].is<const char*>() && !uiPreview(d["preview"].as<String>())) return fail(400, "Unknown screen");
    sendOk();
  });
  server.on("/api/crash/clear", HTTP_POST, [] {
    if (!guard()) return;
    esp_core_dump_image_erase();
    sendOk();
  });
  server.on("/api/settings", HTTP_GET, handleSettingsGet);
  server.on("/api/settings", HTTP_POST, handleSettingsPost);
  server.on("/api/demo", HTTP_POST, handleDemo);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  earnRoutes();
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
}

void webLoop() {
  if (!started && WiFi.status() == WL_CONNECTED && !wifiPortalActive()) {
    MDNS.begin("printquota");
    MDNS.addService("http", "tcp", 80);
    setupRoutes();
    server.begin();
    started = true;
    Serial.printf("admin page: http://printquota.local  (%s)\n", WiFi.localIP().toString().c_str());
  }
  if (!started) return;
  server.handleClient();
  char where[40];
  snprintf(where, sizeof where, "page %s", server.uri().c_str());
  heapMark(where);
}
