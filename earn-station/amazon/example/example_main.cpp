// Integration example only (compile check / reference) - the earn-station has its own main.
// Shows the intended call pattern: credentials and child IDs from NVS, refresh every 30 min,
// stop polling on SIGNED_OUT / BOT_CHECK.
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include "reading_fetch.h"

static Preferences prefs;
static rr::Credentials creds;
static bool blocked = false;           // set on SIGNED_OUT / BOT_CHECK until re-provisioned
static uint32_t lastRun = 0;
static const uint32_t REFRESH_MS = 30UL * 60 * 1000;

struct Kid { const char* name; String id; };
static Kid kids[] = {{"Sam", ""}, {"Alex", ""}};

void setup() {
  Serial.begin(115200);
  prefs.begin("amzread", true);  // read-only; values written by a provisioning step
  creds.atMain = prefs.getString("at_main", "");
  creds.ubidMain = prefs.getString("ubid_main", "");
  kids[0].id = prefs.getString("id_sam", "");
  kids[1].id = prefs.getString("id_alex", "");
  WiFi.begin(prefs.getString("ssid", "").c_str(), prefs.getString("pass", "").c_str());
  prefs.end();
  configTzTime(rr::TZ_POSIX, "pool.ntp.org", "time.nist.gov");
}

void loop() {
  if (blocked || WiFi.status() != WL_CONNECTED || time(nullptr) < 1700000000) return;
  if (lastRun && millis() - lastRun < REFRESH_MS) return;
  lastRun = millis();

  rr::Week week = rr::weekFor(time(nullptr), 0);
  std::vector<rr::KidReport> reports;
  for (Kid& k : kids) {
    rr::KidReport rep;
    rep.name = k.name;
    std::string err;
    rr::Status s = rr::fetchKidWeek(k.id.c_str(), week, creds, rep, err);
    if (s != rr::OK) {
      Serial.printf("reading-report %s: %s %s\n", k.name, rr::statusName(s), err.c_str());
      if (s == rr::SIGNED_OUT || s == rr::BOT_CHECK) blocked = true;  // never retry into a sign-in / captcha
      return;
    }
    Serial.println(rr::summaryLine(rep, week).c_str());
    reports.push_back(rep);
  }
  Serial.println(rr::reportJson(reports, week).c_str());
}
