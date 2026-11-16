// Talks to the print quota device: finds it, pairs once, mirrors its users and
// earning rules, and delivers reading time and chores (queued while it's unreachable).
#include "station.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

std::vector<Person> people;
std::vector<ChoreDef> chores;
Rules rules;
BankState bankState = BankState::NoWifi;
String pairCode, bankError;

static Preferences prefs;
static String bankKey;
static IPAddress bankIp;
static uint32_t nextTryMs = 0, nextFetchMs = 0, nextPairPollMs = 0;
static bool pairAsked = false;

struct Msg {
  uint32_t id = 0;
  int user = -1;
  bool chore = false;
  int32_t seconds = 0;
  bool final = false;
  int32_t sessionS = 0;
  int choreId = 0;
  uint32_t createdMs = 0;
  bool kindle = false;     // from Amazon Kids, not the timer here
  int32_t extraAgeS = 0;   // happened this much earlier than it was queued
};
static std::vector<Msg> queue;
static uint32_t msgBase = 0, msgSeq = 0;
static const char* QUEUE_FILE = "/queue.json";

// ---- Helpers ------------------------------------------------------------------------

Person* findPerson(int id) {
  for (auto& p : people)
    if (p.id == id) return &p;
  return nullptr;
}

ChoreDef* findChore(int id) {
  for (auto& c : chores)
    if (c.id == id) return &c;
  return nullptr;
}

String fmtDur(int32_t s) {
  char buf[24];
  const char* sign = s < 0 ? "-" : "";
  uint32_t a = s < 0 ? -(int64_t)s : s;
  if (a >= 3600) snprintf(buf, sizeof buf, "%s%luh %02lum", sign, a / 3600, (a / 60) % 60);
  else snprintf(buf, sizeof buf, "%s%lum", sign, a / 60);
  return buf;
}

String money(int32_t c) {
  char buf[20];
  snprintf(buf, sizeof buf, "%s$%ld.%02ld", c < 0 ? "-" : "", (long)abs(c) / 100, (long)abs(c) % 100);
  return buf;
}

int32_t weekSecondsLeft() {
  if (!rules.weekEnd || !rules.now) return -1;
  int64_t now = rules.now + (millis() - rules.fetchedMs) / 1000;
  return max<int64_t>(0, rules.weekEnd - now);
}

size_t queuedCount() { return queue.size(); }

static void saveQueue() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (auto& m : queue) {
    JsonObject o = a.add<JsonObject>();
    o["id"] = m.id;
    o["u"] = m.user;
    o["c"] = m.chore;
    o["s"] = m.seconds;
    o["f"] = m.final;
    o["ss"] = m.sessionS;
    o["ch"] = m.choreId;
    if (m.kindle) o["k"] = true;
    if (m.extraAgeS) o["xa"] = m.extraAgeS;
  }
  File f = LittleFS.open(QUEUE_FILE, "w");
  if (!f) return;
  serializeJson(d, f);
  f.close();
}

static void loadQueue() {
  File f = LittleFS.open(QUEUE_FILE, "r");
  if (!f) return;
  JsonDocument d;
  if (!deserializeJson(d, f)) {
    for (JsonObject o : d.as<JsonArray>()) {
      Msg m;
      m.id = o["id"];
      m.user = o["u"];
      m.chore = o["c"];
      m.seconds = o["s"];
      m.final = o["f"];
      m.sessionS = o["ss"];
      m.choreId = o["ch"];
      m.kindle = o["k"] | false;
      m.extraAgeS = o["xa"] | 0;
      m.createdMs = millis();
      queue.push_back(m);
    }
  }
  f.close();
  Serial.printf("%u queued items from before restart\n", (unsigned)queue.size());
}

static void enqueue(Msg m) {
  m.id = msgBase + ++msgSeq;
  m.createdMs = millis();
  if (queue.size() >= 200) queue.erase(queue.begin());
  queue.push_back(m);
  saveQueue();
  nextTryMs = 0;  // try to deliver right away
}

void queueRead(int userId, int32_t s, bool final, int32_t sessionS) {
  if (s <= 0 && !final) return;
  Msg m;
  m.user = userId;
  m.seconds = max<int32_t>(0, s);
  m.final = final;
  m.sessionS = sessionS;
  enqueue(m);
}

// The bank takes at most an hour per message: send longer stretches in pieces, the last one
// carrying the total so it shows as one entry in the history.
void queueKindle(int userId, int32_t s, int32_t extraAgeS) {
  for (int32_t left = s; left > 0;) {
    Msg m;
    m.user = userId;
    m.seconds = min<int32_t>(left, 3600);
    left -= m.seconds;
    m.final = left == 0;
    m.sessionS = s;
    m.kindle = true;
    m.extraAgeS = extraAgeS;
    enqueue(m);
  }
}

void queueChore(int userId, int choreId) {
  Msg m;
  m.user = userId;
  m.chore = true;
  m.choreId = choreId;
  enqueue(m);
  // Count it locally right away so the limit shows before the bank answers
  if (Person* p = findPerson(userId)) p->choresToday[choreId]++;
  uiDirty = true;
}

void bankRefreshSoon() { nextFetchMs = 0; }

// ---- HTTP -------------------------------------------------------------------------

#ifdef WOKWI
static String url(const char* path) { return String("http://") + BANK_HOST + path; }
#else
static String url(const char* path) { return "http://" + bankIp.toString() + path; }
#endif

static int request(const char* method, const char* path, const String& body, String& out) {
  HTTPClient http;
  http.setConnectTimeout(2500);
  http.setTimeout(3000);
  if (!http.begin(url(path))) return -1;
  if (bankKey.length()) http.addHeader("X-Earn-Key", bankKey);
  int code;
  if (!strcmp(method, "GET")) {
    code = http.GET();
  } else {
    http.addHeader("Content-Type", "application/json");
    code = http.POST(body);
  }
  out = code > 0 ? http.getString() : String();
  http.end();
  return code;
}

static void goOffline(const String& why) {
  if (bankState != BankState::Offline) Serial.printf("bank offline: %s\n", why.c_str());
  bankState = BankState::Offline;
  bankError = why;
  nextTryMs = millis() + 15000;  // each try can block the screen for a few seconds
  uiDirty = true;
}

static bool findBank() {
#ifdef WOKWI
  bankIp = IPAddress(1, 1, 1, 1);  // unused: requests go to BANK_HOST
  return true;
#endif
  IPAddress ip = MDNS.queryHost("printquota", 2500);
  if (ip == IPAddress(0, 0, 0, 0)) {
    String saved = prefs.getString("ip", "");
    if (saved.isEmpty() || !ip.fromString(saved)) return false;
  } else if (ip.toString() != prefs.getString("ip", "")) {
    prefs.putString("ip", ip.toString());
  }
  bankIp = ip;
  Serial.printf("bank at %s\n", bankIp.toString().c_str());
  return true;
}

// ---- Pairing ------------------------------------------------------------------------

static void pairStep() {
  String out;
  if (!pairAsked) {
    pairCode = String(1000 + esp_random() % 9000);
    JsonDocument d;
    d["code"] = pairCode;
    d["name"] = "Earning station";
    String body;
    serializeJson(d, body);
    int code = request("POST", "/api/earn/pair", body, out);
    if (code != 200) {
      goOffline(code == 429 ? "Another device is pairing" : "Can't reach printquota.local");
      return;
    }
    pairAsked = true;
    bankState = BankState::Pairing;
    uiDirty = true;
    nextPairPollMs = millis() + 2000;
    return;
  }
  if (millis() < nextPairPollMs) return;
  nextPairPollMs = millis() + 2000;
  int code = request("GET", ("/api/earn/pair?code=" + pairCode).c_str(), "", out);
  if (code != 200) {
    goOffline("Can't reach printquota.local");
    return;
  }
  JsonDocument d;
  deserializeJson(d, out);
  String st = d["status"] | "";
  if (st == "allowed") {
    bankKey = d["key"] | "";
    prefs.putString("key", bankKey);
    Serial.println("paired");
    bankState = BankState::Offline;  // fetch next
    nextTryMs = nextFetchMs = 0;
    uiDirty = true;
  } else if (st == "denied") {
    bankState = BankState::Denied;
    pairAsked = false;
    uiDirty = true;
  } else if (st == "unknown") {
    pairAsked = false;  // expired or the bank restarted: ask again
  }
}

void bankForget() {
  bankKey = "";
  prefs.remove("key");
  pairAsked = false;
  bankState = BankState::Finding;
  nextTryMs = 0;
  uiDirty = true;
}

// ---- Sync ------------------------------------------------------------------------------

static bool fetchState() {
  String out;
  int code = request("GET", "/api/earn/state", "", out);
  if (code == 401) {
    Serial.println("bank no longer knows this station; pairing again");
    bankForget();
    return false;
  }
  if (code != 200) {
    goOffline(code < 0 ? "Can't reach printquota.local" : "Print quota error " + String(code));
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, out)) return false;
  rules.printPct = d["print_pct"] | 200;
  rules.filRate = d["fil_rate"] | 5000;
  rules.cashRate = d["cash_rate"] | 10000;
  rules.goalS = d["goal_s"] | 7 * 3600;
  rules.checkMin = d["check_min"] | 30;
  rules.weekEnd = d["week_end"] | 0;
  rules.now = d["time_ok"] ? (d["now"] | 0) : 0;
  rules.fetchedMs = millis();

  std::vector<Person> np;
  for (JsonObject o : d["users"].as<JsonArray>()) {
    Person p;
    p.id = o["id"];
    p.name = o["name"] | "";
    p.leftS = o["left_s"];
    p.unlimited = o["unlimited"];
    p.filC = o["fil_c"];
    p.cashC = o["cash_c"];
    p.weekReadS = o["week_read_s"];
    p.weekPendingC = o["week_pending_c"];
    p.weekPaidC = o["week_paid_c"];
    for (JsonPair kv : o["chores_today"].as<JsonObject>()) p.choresToday[atoi(kv.key().c_str())] = kv.value();
    np.push_back(p);
  }
  std::vector<ChoreDef> nc;
  for (JsonObject o : d["chores"].as<JsonArray>()) {
    ChoreDef c;
    c.id = o["id"];
    c.name = o["name"] | "";
    c.printS = o["print_s"];
    c.filC = o["fil_c"];
    c.cashC = o["cash_c"];
    c.perDay = o["per_day"];
    nc.push_back(c);
  }
  people = np;
  chores = nc;
  // Sessions of people who were removed end quietly
  for (size_t i = 0; i < sessions.size();)
    if (!findPerson(sessions[i].userId)) sessions.erase(sessions.begin() + i);
    else i++;
  // Redraw only when something shown changed (the bank's clock always does)
  static String lastSig;
  d.remove("now");
  String sig;
  serializeJson(d, sig);
  if (sig != lastSig || bankState != BankState::Ok) uiDirty = true;
  lastSig = sig;
  if (bankState != BankState::Ok) Serial.println("bank connected");
  bankState = BankState::Ok;
  bankError = "";
  return true;
}

// Deliver queued items, oldest first. Returns false if the bank couldn't be reached.
static bool deliver() {
  bool sent = false;
  while (!queue.empty()) {
    Msg& m = queue.front();
    JsonDocument d;
    d["msg"] = m.id;
    d["user"] = m.user;
    if (m.chore) {
      d["kind"] = "chore";
      d["chore"] = m.choreId;
    } else {
      d["kind"] = "read";
      d["seconds"] = m.seconds;
      d["final"] = m.final;
      d["session_s"] = m.sessionS;
    }
    d["age_s"] = (millis() - m.createdMs) / 1000 + m.extraAgeS;
    if (m.kindle) d["src"] = "kindle";
    String body, out;
    serializeJson(d, body);
    int code = request("POST", "/api/earn/add", body, out);
    if (code == 401) {
      bankForget();
      return false;
    }
    if (code < 0 || code >= 500) {
      goOffline("Can't reach printquota.local");
      return false;
    }
    if (code != 200) Serial.printf("bank refused item (%d): %s\n", code, out.c_str());
    queue.erase(queue.begin());  // delivered, or refused for good (e.g. chore limit)
    sent = true;
  }
  if (sent) {
    saveQueue();
    nextFetchMs = 0;
  }
  return true;
}

void bankBegin() {
  prefs.begin("earn", false);
  bankKey = prefs.getString("key", "");
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  msgBase = esp_random() & 0xFFF00000;  // so ids differ across restarts
  loadQueue();
}

void bankLoop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (bankState != BankState::NoWifi) {
      bankState = BankState::NoWifi;
      uiDirty = true;
    }
    return;
  }
  static bool mdnsUp = false;
  if (!mdnsUp) {
#ifdef WOKWI
    mdnsUp = true;
#else
    mdnsUp = MDNS.begin("earnstation");
#endif
    bankState = BankState::Finding;
    uiDirty = true;
  }
  if (bankState == BankState::Denied || millis() < nextTryMs) return;

  if (bankIp == IPAddress(0, 0, 0, 0) || bankState == BankState::Finding) {
    if (!findBank()) {
      bankState = BankState::Finding;
      bankError = "Looking for printquota.local";
      nextTryMs = millis() + 5000;
      return;
    }
    if (bankState == BankState::Finding) bankState = BankState::Offline;
  }

  if (bankKey.isEmpty()) {
    pairStep();
    return;
  }

  if (!queue.empty() && !deliver()) return;
  if (millis() >= nextFetchMs) {
    nextFetchMs = millis() + 10000;
    if (!fetchState() && bankState == BankState::Offline) {
      bankIp = IPAddress(0, 0, 0, 0);  // the address may have changed: look it up again next time
    }
  }
}

bool bankPaired() { return bankKey.length() > 0; }
bool bankKeyMatches(const String& k) { return bankKey.length() >= 16 && k == bankKey; }

void bankStatus() {
  const char* st[] = {"no wifi", "finding", "pairing", "denied", "ok", "offline"};
  Serial.printf("bank: %s %s ip=%s paired=%s queued=%u\n", st[(int)bankState], bankError.c_str(),
                bankIp.toString().c_str(), bankKey.length() ? "yes" : "no", (unsigned)queue.size());
  for (auto& p : people)
    Serial.printf("  %-10s left %s fil %s cash %s week %s pending %s\n", p.name.c_str(), fmtDur(p.leftS).c_str(),
                  money(p.filC).c_str(), money(p.cashC).c_str(), fmtDur(p.weekReadS).c_str(),
                  money(p.weekPendingC).c_str());
  for (auto& s : sessions)
    Serial.printf("  reading: user %d %lds%s\n", s.userId, (long)sessionSeconds(s), s.asking ? " (asking)" : "");
}
