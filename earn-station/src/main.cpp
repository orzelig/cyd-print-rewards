// Earning station for the print quota device, on a second "Cheap Yellow Display".
//
// Kids tap their name, then either start a reading timer or log a finished chore.
// The time and money they earn go to the print quota device (printquota.local), which
// keeps all balances and rules. Earning settings and chores are edited there.
//
// First boot: join the Wi-Fi network "EarnStation-Setup" on your phone and pick your home
// Wi-Fi. Then open printquota.local and tap Allow when the station asks to connect.
// Hold the BOOT button for 5 seconds to forget the Wi-Fi.
#include "station.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include "amazon/reading_core.h"

static WiFiManager wm;
constexpr int BOOT_BTN = 0;

#ifdef WOKWI
bool wifiPortalActive() { return false; }
#else
bool wifiPortalActive() { return wm.getConfigPortalActive(); }
#endif

static void pollBootButton() {
  static uint32_t pressedAt = 0;
  if (digitalRead(BOOT_BTN) != LOW) {
    pressedAt = 0;
    return;
  }
  if (!pressedAt) pressedAt = millis();
  else if (millis() - pressedAt > 5000) {
    wm.resetSettings();
    delay(500);
    ESP.restart();
  }
}

// USB serial console (115200 baud)
static void handleCommand(const String& cmd) {
  if (cmd == "status") {
    Serial.println("version " FW_VERSION);
    bankStatus();
  } else if (cmd == "unpair") {
    bankForget();
    Serial.println("OK: pairing removed");
  } else if (cmd.startsWith("tap ")) {
    int x, y;
    if (sscanf(cmd.c_str() + 4, "%d %d", &x, &y) == 2) uiTap(x, y);
  } else {
    Serial.println("commands: status, unpair, tap X Y");
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
  Serial.println("Earning station starting");
  // Create the system console's write lock now; if the first console write is a network-task
  // error during a low-memory moment, creating it fails and the device aborts.
  printf("firmware %s\n", FW_VERSION);
  fflush(stdout);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  uiBegin();
  bankBegin();
  amazonBegin();

  WiFi.mode(WIFI_STA);
  configTzTime(rr::TZ_POSIX, "pool.ntp.org", "time.google.com");  // for Amazon: TLS and the reading week
#ifdef WOKWI
  WiFi.begin("Wokwi-GUEST", "", 6);  // the emulator's virtual network
  return;
#endif
  wm.setHostname("earnstation");
  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(15);
  wm.autoConnect("EarnStation-Setup");
}

void loop() {
#ifndef WOKWI
  wm.process();
#endif
  bankLoop();
  amazonLoop();
  webLoop();
  readingLoop();
  uiLoop();
  pollBootButton();
  pollSerial();
  delay(2);
}
