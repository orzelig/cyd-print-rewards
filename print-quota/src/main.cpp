// Print quota manager for a Bambu Lab printer, on the ESP32 "Cheap Yellow Display".
//
// - Each user has a print-time allowance that resets daily/weekly/monthly (or never),
//   plus an extra balance you can top up.
// - At the device, people tap their name and then start their print. The time the
//   printer actually spends printing is charged to them (read live from the printer).
// - Manage users, quotas, history and settings at http://printquota.local
//
// First boot: join the Wi-Fi network "PrintQuota-Setup" on your phone and pick your
// home Wi-Fi. Hold the BOOT button for 5 seconds to forget the Wi-Fi.
#include "app.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <time.h>

static WiFiManager wm;

char heapLowAt[48] = "start";
void heapMark(const char* where) {
  static uint32_t seen = UINT32_MAX;
  uint32_t m = ESP.getMinFreeHeap();
  if (m >= seen) return;
  seen = m;
  snprintf(heapLowAt, sizeof heapLowAt, "%s (%u free)", where, (unsigned)m);
}
constexpr int BOOT_BTN = 0;

bool wifiPortalActive() { return wm.getConfigPortalActive(); }

String netStatus() {
  if (WiFi.status() == WL_CONNECTED) return "printquota.local";
  if (wm.getConfigPortalActive()) return "Wi-Fi setup: PrintQuota-Setup";
  return "Connecting Wi-Fi...";
}

void applySettings() {
  configTzTime(settings.tz.c_str(), "pool.ntp.org", "time.google.com");
  printerReconfigure();
}

static void pollBootButton() {
  static uint32_t pressedAt = 0;
  if (digitalRead(BOOT_BTN) != LOW) {
    pressedAt = 0;
    return;
  }
  if (!pressedAt) pressedAt = millis();
  else if (millis() - pressedAt > 5000) {
    saveUsers();
    wm.resetSettings();
    delay(500);
    ESP.restart();
  }
}

// USB serial console (115200 baud) for recovery when you have physical access
static void handleCommand(const String& cmd) {
  if (cmd == "clear-admin-password") {
    settings.adminPass = "";
    saveSettings();
    Serial.println("OK: admin password cleared");
  } else if (cmd == "status") {
    Serial.printf("printer: %s %s\n", pstateName(printer.state), printer.error.c_str());
    Serial.printf("access code set: %s, admin password set: %s\n", settings.accessCode.length() ? "yes" : "no",
                  settings.adminPass.length() ? "yes" : "no");
    for (auto& u : users)
      Serial.printf("  %-12s left %s\n", u.name.c_str(), u.quotaS < 0 ? "no limit" : fmtDur(remainingS(u)).c_str());
  } else if (cmd.startsWith("tap ")) {
    int x, y;
    if (sscanf(cmd.c_str() + 4, "%d %d", &x, &y) == 2) {
      uiTap(x, y);
      Serial.printf("claim=%d job.user=%d\n", claimUserId, job.userId);
    }
  } else {
    Serial.println("commands: status, clear-admin-password, tap X Y");
  }
}

static void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line.length()) handleCommand(line);
      line = "";
    } else if (line.length() < 64) {
      line += c;
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("Print quota starting");
  // Write once through the system console now, while memory is plentiful. Its write lock is
  // created on first use; if the first use is an error message logged by the network task
  // during a low-memory moment, creating the lock fails and the whole device aborts.
  printf("firmware %s\n", FW_VERSION);
  fflush(stdout);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  storeBegin();
  uiBegin();

  WiFi.mode(WIFI_STA);
  wm.setHostname("printquota");
  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(15);
  wm.autoConnect("PrintQuota-Setup");  // opens the setup network if no Wi-Fi is saved

  heapMark("setup");
  printerBegin();
  applySettings();
  heapMark("setup printer");
}

void loop() {
  heapMark("between loops");
  wm.process();
  heapMark("wifi");
  printerLoop();
  heapMark("printer");
  tickQuota();
  heapMark("quota");
  webLoop();  // marks the page it served
  uiLoop();
  heapMark("screen");
  pollBootButton();
  pollSerial();
  delay(2);
}
