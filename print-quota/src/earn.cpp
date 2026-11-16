// Earning: reading time and chores reported by the earning station.
//
// Reading, per minute (all configurable on the admin page):
//   - print time: 2 minutes per minute read, added right away
//   - filament money: $0.50 per hour read, added right away
//   - cash: $1.00 per hour read, but only once the person has read 7 hours in the
//     week (Sunday 00:00 to Saturday midnight). Until then it is "pending".
//     Last week stays open until Sunday 8 am, so Kindle reading from Saturday evening
//     (fetched Sunday morning) still counts toward it. Then, if the goal wasn't
//     reached, the pending cash is dropped.
// Chores add a fixed reward each, up to a number of times per day.
//
// The station pairs once (approved on the admin page) and then sends its key with
// every request.
#include "app.h"
#include "web_util.h"
#include <LittleFS.h>
#include <time.h>

EarnRules earn;

static const char* EARN_FILE = "/earn.json";

struct PairRequest {
  bool active = false;
  String code, name, ip, key;
  int status = 0;  // 0 waiting, 1 allowed, 2 denied
  uint32_t atMs = 0;
};
static PairRequest pair;

static uint32_t seenIds[64];
static uint8_t seenPos = 0;

// ---- Time --------------------------------------------------------------------------

constexpr uint32_t PREV_WEEK_OPEN_S = 8 * 3600;  // last week closes Sunday 8 am

static time_t weekStartFor(time_t now) {
  struct tm t;
  localtime_r(&now, &t);
  t.tm_hour = t.tm_min = t.tm_sec = 0;
  t.tm_mday -= t.tm_wday;  // back to Sunday
  t.tm_isdst = -1;
  return mktime(&t);
}

uint32_t weekEndEpoch() {
  if (!earn.weekStart) return 0;
  time_t ws = earn.weekStart;
  struct tm t;
  localtime_r(&ws, &t);
  t.tm_mday += 7;
  t.tm_isdst = -1;
  return mktime(&t);
}

static uint32_t dayKey() {
  if (!timeValid()) return 0;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

void choreCountsToday(User& u) {
  uint32_t k = dayKey();
  if (k && u.choreDay != k) {
    u.choreDay = k;
    u.choreCount.clear();
  }
}

// ---- Money -------------------------------------------------------------------------

static int32_t cashFor(int32_t readS) { return (int64_t)readS * earn.cashRate / 360000; }
static int32_t filFor(int32_t readS) { return (int64_t)readS * earn.filRate / 360000; }

static int32_t pendingFor(int32_t readS) { return readS >= earn.goalS ? 0 : cashFor(readS); }
int32_t weekPendingC(const User& u) { return pendingFor(u.weekReadS); }

uint32_t prevWeekClosesEpoch() { return earn.prevWeekStart ? earn.weekStart + PREV_WEEK_OPEN_S : 0; }

static void addFilament(User& u, int32_t readS) {
  int64_t units = (int64_t)readS * earn.filRate / 3600 + u.filFrac;  // 1/100 cent
  u.filC += units / 100;
  u.filFrac = units % 100;
}

// Pay out a week's cash once its goal is reached (and any reading after that)
static void settleCash(User& u, int32_t readS, int32_t& paidC, bool lastWeek) {
  if (readS < earn.goalS) return;
  int32_t due = cashFor(readS);
  if (due <= paidC) return;
  bool first = paidC == 0;
  u.cashC += due - paidC;
  if (first) {
    JsonDocument e;
    e["type"] = "goal";
    e["t"] = nowEpoch();
    e["user"] = u.name;
    e["s"] = readS;
    e["cash"] = due;
    if (lastWeek) e["note"] = "last week";
    logHistory(e);
    Serial.printf("%s reached the weekly reading goal: $%.2f\n", u.name.c_str(), due / 100.0);
  }
  paidC = due;
}
static void settleCash(User& u) { settleCash(u, u.weekReadS, u.weekPaidC, false); }

// Last week is over for good: drop its pending cash
static void closePrevWeek() {
  for (auto& u : users) {
    settleCash(u, u.prevReadS, u.prevPaidC, true);
    int32_t lost = pendingFor(u.prevReadS);
    if (lost > 0) {
      JsonDocument e;
      e["type"] = "missed";
      e["t"] = nowEpoch();
      e["user"] = u.name;
      e["s"] = u.prevReadS;
      e["cash"] = lost;
      logHistory(e);
    }
    u.prevReadS = 0;
    u.prevPaidC = 0;
  }
  earn.prevWeekStart = 0;
  Serial.println("last week closed");
  usersChanged();
  saveEarn();
}

static void checkWeek() {
  if (!timeValid()) return;
  uint32_t now = time(nullptr);
  uint32_t ws = weekStartFor(now);
  if (ws != earn.weekStart) {
    if (earn.prevWeekStart) closePrevWeek();  // (only if the device missed a Sunday morning)
    if (earn.weekStart) {
      bool lastWeek = weekEndEpoch() == ws;   // else the device was off for more than a week
      for (auto& u : users) {
        u.prevReadS = lastWeek ? u.weekReadS : 0;
        u.prevPaidC = lastWeek ? u.weekPaidC : 0;
        if (!lastWeek) settleCash(u);
        u.weekReadS = 0;
        u.weekPaidC = 0;
      }
      earn.prevWeekStart = lastWeek ? earn.weekStart : 0;
      Serial.println("new reading week");
      usersChanged();
    }
    earn.weekStart = ws;
    saveEarn();
  }
  if (earn.prevWeekStart && now >= prevWeekClosesEpoch()) closePrevWeek();
}

void earnTick() {
  checkWeek();
  if (pair.active && millis() - pair.atMs > 10UL * 60 * 1000) pair = PairRequest();
}

// ---- Persistence --------------------------------------------------------------------

static void choreJson(JsonObject o, const Chore& c) {
  o["id"] = c.id;
  o["name"] = c.name;
  o["print_s"] = c.printS;
  o["fil_c"] = c.filC;
  o["cash_c"] = c.cashC;
  o["per_day"] = c.perDay;
}

static Chore choreFrom(JsonObject o) {
  Chore c;
  c.id = o["id"] | 0;
  c.name = o["name"] | "";
  c.name.trim();
  if (c.name.length() > 28) c.name = c.name.substring(0, 28);
  c.printS = constrain(o["print_s"] | 0, 0, 100 * 3600);
  c.filC = constrain(o["fil_c"] | 0, 0, 100000);
  c.cashC = constrain(o["cash_c"] | 0, 0, 100000);
  c.perDay = constrain(o["per_day"] | 1, 0, 20);
  return c;
}

void saveEarn() {
  JsonDocument d;
  d["print_pct"] = earn.printPct;
  d["fil_rate"] = earn.filRate;
  d["cash_rate"] = earn.cashRate;
  d["goal_s"] = earn.goalS;
  d["check_min"] = earn.checkMin;
  d["week_start"] = earn.weekStart;
  d["prev_week_start"] = earn.prevWeekStart;
  d["per_hour"] = true;
  JsonArray ch = d["chores"].to<JsonArray>();
  for (auto& c : earn.chores) choreJson(ch.add<JsonObject>(), c);
  JsonArray dv = d["devices"].to<JsonArray>();
  for (auto& x : earn.devices) {
    JsonObject o = dv.add<JsonObject>();
    o["name"] = x.name;
    o["key"] = x.key;
  }
  File f = LittleFS.open("/earn.json.tmp", "w");
  if (!f) return;
  serializeJson(d, f);
  f.close();
  LittleFS.remove(EARN_FILE);
  LittleFS.rename("/earn.json.tmp", EARN_FILE);
}

static void addDefaultChores() {
  const struct { const char* name; int min; } defs[] = {
      {"Empty the dishwasher", 15}, {"Clean your room", 30}, {"Take out the trash", 10},
      {"Fold laundry", 20},         {"Help make dinner", 20},
  };
  uint16_t id = 1;
  for (auto& d : defs) {
    Chore c;
    c.id = id++;
    c.name = d.name;
    c.printS = d.min * 60;
    earn.chores.push_back(c);
  }
}

void earnBegin() {
  File f = LittleFS.open(EARN_FILE, "r");
  JsonDocument d;
  if (!f || deserializeJson(d, f)) {
    if (f) f.close();
    addDefaultChores();
    saveEarn();
    return;
  }
  f.close();
  earn.printPct = d["print_pct"] | 200;
  earn.filRate = d["fil_rate"] | 5000;
  earn.cashRate = d["cash_rate"] | 10000;
  if (!(d["per_hour"] | false)) {  // saved by an older version: rates were per minute
    earn.filRate = min<uint32_t>(earn.filRate * 60, 100000000);
    earn.cashRate = min<uint32_t>(earn.cashRate * 60, 100000000);
  }
  earn.prevWeekStart = d["prev_week_start"] | 0;
  earn.goalS = d["goal_s"] | 7 * 3600;
  earn.checkMin = d["check_min"] | 30;
  earn.weekStart = d["week_start"] | 0;
  for (JsonObject o : d["chores"].as<JsonArray>()) {
    Chore c = choreFrom(o);
    if (c.id && c.name.length()) earn.chores.push_back(c);
  }
  for (JsonObject o : d["devices"].as<JsonArray>()) earn.devices.push_back({o["name"] | "", o["key"] | ""});
}

// ---- JSON ----------------------------------------------------------------------------

void earnUserJson(JsonObject o, User& u) {
  choreCountsToday(u);
  o["earns"] = u.earns;
  o["fil_c"] = u.filC;
  o["cash_c"] = u.cashC;
  o["week_read_s"] = u.weekReadS;
  o["week_pending_c"] = weekPendingC(u);
  o["week_paid_c"] = u.weekPaidC;
  if (earn.prevWeekStart) {
    o["prev_read_s"] = u.prevReadS;
    o["prev_pending_c"] = pendingFor(u.prevReadS);
    o["prev_paid_c"] = u.prevPaidC;
  }
  o["read_total_s"] = u.readTotalS;
  JsonObject ct = o["chores_today"].to<JsonObject>();
  for (auto& kv : u.choreCount) ct[String(kv.first)] = kv.second;
}

static void rulesJson(JsonDocument& d) {
  d["print_pct"] = earn.printPct;
  d["fil_rate"] = earn.filRate;
  d["cash_rate"] = earn.cashRate;
  d["goal_s"] = earn.goalS;
  d["check_min"] = earn.checkMin;
  d["week_end"] = weekEndEpoch();
  d["prev_closes"] = prevWeekClosesEpoch();
  JsonArray ch = d["chores"].to<JsonArray>();
  for (auto& c : earn.chores) choreJson(ch.add<JsonObject>(), c);
}

// ---- Station API (needs the station key) ----------------------------------------------

static bool stationAuth() {
  String k = server.header("X-Earn-Key");
  if (k.length() >= 16)
    for (auto& d : earn.devices)
      if (d.key == k) {
        d.ip = server.client().remoteIP().toString();
        return true;
      }
  fail(401, "Not paired");
  return false;
}

static void handleStationState() {
  if (!stationAuth()) return;
  JsonDocument d;
  rulesJson(d);
  d["now"] = nowEpoch();
  d["time_ok"] = timeValid();
  JsonArray a = d["users"].to<JsonArray>();
  for (auto& u : users) {
    if (!u.earns) continue;  // not shown on the station
    JsonObject o = a.add<JsonObject>();
    o["id"] = u.id;
    o["name"] = u.name;
    o["left_s"] = u.quotaS < 0 ? 0 : remainingS(u);
    o["unlimited"] = u.quotaS < 0;
    earnUserJson(o, u);
  }
  sendJson(d);
}

static bool seen(uint32_t id) {
  for (uint32_t s : seenIds)
    if (s == id) return true;
  seenIds[seenPos++ % 64] = id;
  return false;
}

static void handleStationAdd() {
  if (!stationAuth()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  uint32_t msgId = d["msg"] | 0;
  User* u = findUser(d["user"] | -1);
  if (!u) return fail(404, "No such person");
  if (msgId && seen(msgId)) return sendOk();  // a retry of something already counted

  String kind = d["kind"] | "";
  if (kind == "read") {
    int32_t s = d["seconds"] | 0;
    if (s < 0 || s > 3600) return fail(400, "Bad reading time");
    uint32_t happened = nowEpoch() - min<uint32_t>(d["age_s"] | 0, nowEpoch());
    u->balanceS += (int64_t)s * earn.printPct / 100;
    addFilament(*u, s);
    u->readTotalS += s;
    if (!timeValid() || happened >= earn.weekStart) {
      u->weekReadS += s;
      settleCash(*u);
    } else if (earn.prevWeekStart && happened >= earn.prevWeekStart) {  // last week, still open
      u->prevReadS += s;
      settleCash(*u, u->prevReadS, u->prevPaidC, true);
    }  // older than that: earns print time and filament, but no longer counts toward a goal
    int32_t session = d["session_s"] | 0;
    if ((d["final"] | false) && session >= 60) {
      JsonDocument e;
      e["type"] = "read";
      if (strcmp(d["src"] | "", "kindle") == 0) e["note"] = "Kindle";
      e["t"] = nowEpoch() - session;
      e["user"] = u->name;
      e["s"] = session;
      e["print"] = (int64_t)session * earn.printPct / 100;
      e["fil"] = filFor(session);
      e["week"] = happened >= earn.weekStart || !earn.prevWeekStart ? u->weekReadS : u->prevReadS;
      logHistory(e);
    }
  } else if (kind == "chore") {
    int cid = d["chore"] | -1;
    Chore* c = nullptr;
    for (auto& x : earn.chores)
      if (x.id == cid) c = &x;
    if (!c) return fail(404, "That chore was removed");
    choreCountsToday(*u);
    uint8_t& n = u->choreCount[c->id];
    if (c->perDay && n >= c->perDay) return fail(409, "Already done today");
    n++;
    u->balanceS += c->printS;
    u->filC += c->filC;
    u->cashC += c->cashC;
    JsonDocument e;
    e["type"] = "chore";
    e["t"] = nowEpoch();
    e["user"] = u->name;
    e["note"] = c->name;
    e["s"] = c->printS;
    e["fil"] = c->filC;
    e["cash"] = c->cashC;
    logHistory(e);
  } else {
    return fail(400, "Unknown kind");
  }
  usersChanged();
  sendOk();
}

// ---- Pairing ------------------------------------------------------------------------------

static void handlePairRequest() {
  JsonDocument d;
  if (!readBody(d)) return;
  String code = d["code"] | "";
  if (code.length() != 4) return fail(400, "Bad code");
  if (pair.active && pair.status == 0 && pair.code != code && millis() - pair.atMs < 15000)
    return fail(429, "Busy");
  if (pair.active && pair.code == code) return sendOk();  // already asked
  pair = PairRequest();
  pair.active = true;
  pair.code = code;
  pair.name = d["name"] | "Earning station";
  if (pair.name.length() > 24) pair.name = pair.name.substring(0, 24);
  pair.ip = server.client().remoteIP().toString();
  pair.atMs = millis();
  Serial.printf("pairing request from %s, code %s\n", pair.ip.c_str(), code.c_str());
  sendOk();
}

static void handlePairPoll() {
  JsonDocument d;
  if (!pair.active || server.arg("code") != pair.code) {
    d["status"] = "unknown";
  } else if (pair.status == 1) {
    d["status"] = "allowed";
    d["key"] = pair.key;
    pair = PairRequest();  // hand the key out once
  } else if (pair.status == 2) {
    d["status"] = "denied";
    pair = PairRequest();
  } else {
    d["status"] = "waiting";
  }
  sendJson(d);
}

static String newKey() {
  char buf[33];
  snprintf(buf, sizeof buf, "%08lx%08lx%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random(),
           (unsigned long)esp_random(), (unsigned long)esp_random());
  return buf;
}

static void handlePairAnswer() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  if (!pair.active || pair.status != 0) return fail(409, "No device is waiting to connect");
  if (d["allow"] | false) {
    if (earn.devices.size() >= 4) earn.devices.erase(earn.devices.begin());
    pair.key = newKey();
    earn.devices.push_back({pair.name, pair.key});
    pair.status = 1;
    saveEarn();
  } else {
    pair.status = 2;
  }
  sendOk();
}

// ---- Admin API ----------------------------------------------------------------------------

static void handleEarnSettingsGet() {
  if (!guard()) return;
  JsonDocument d;
  rulesJson(d);
  JsonArray dv = d["devices"].to<JsonArray>();
  for (auto& x : earn.devices) {
    JsonObject o = dv.add<JsonObject>();
    o["name"] = x.name;
    o["ip"] = x.ip;
  }
  if (pair.active && pair.status == 0) {
    d["pair"]["code"] = pair.code;
    d["pair"]["name"] = pair.name;
    d["pair"]["ip"] = pair.ip;
  }
  sendJson(d);
}

static void handleEarnSettingsPost() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  if (d["print_pct"].is<int>()) earn.printPct = constrain(d["print_pct"].as<int>(), 0, 1000);
  if (d["fil_rate"].is<int>()) earn.filRate = constrain(d["fil_rate"].as<int>(), 0, 1000000);
  if (d["cash_rate"].is<int>()) earn.cashRate = constrain(d["cash_rate"].as<int>(), 0, 1000000);
  if (d["goal_s"].is<int>()) earn.goalS = constrain(d["goal_s"].as<int>(), 0, 100 * 3600);
  if (d["check_min"].is<int>()) earn.checkMin = constrain(d["check_min"].as<int>(), 0, 240);
  if (d["chores"].is<JsonArray>()) {
    std::vector<Chore> list;
    uint16_t next = 1;
    for (auto& c : earn.chores) next = max<uint16_t>(next, c.id + 1);
    for (JsonObject o : d["chores"].as<JsonArray>()) {
      Chore c = choreFrom(o);
      if (c.name.isEmpty()) continue;
      if (!c.id) c.id = next++;
      list.push_back(c);
      if (list.size() >= 30) break;
    }
    earn.chores = list;
  }
  for (auto& u : users) settleCash(u);  // a lower goal may already be met
  saveEarn();
  usersChanged();
  sendOk();
}

static void handleDeviceRemove() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  int i = d["index"] | -1;
  if (i < 0 || i >= (int)earn.devices.size()) return fail(404, "No such device");
  earn.devices.erase(earn.devices.begin() + i);
  saveEarn();
  sendOk();
}

// Add or take money from a wallet (buying filament, paying out cash, corrections)
static void handleWallet() {
  if (!guard()) return;
  JsonDocument d;
  if (!readBody(d)) return;
  User* u = findUser(d["id"] | -1);
  if (!u) return fail(404, "No such user");
  String w = d["wallet"] | "";
  int32_t c = d["cents"] | 0;
  if (c == 0 || abs(c) > 10000000) return fail(400, "Enter an amount");
  if (w == "fil") u->filC += c;
  else if (w == "cash") u->cashC += c;
  else return fail(400, "Unknown wallet");
  String note = d["note"] | "";
  note.trim();
  JsonDocument e;
  e["type"] = "wallet";
  e["t"] = nowEpoch();
  e["user"] = u->name;
  e["wallet"] = w;
  e["cents"] = c;
  e["note"] = note.substring(0, 60);
  e["bal"] = w == "fil" ? u->filC : u->cashC;
  logHistory(e);
  usersChanged();
  sendOk();
}

// For updating a station's firmware over Wi-Fi: its address and key (admin only)
static void handleDeviceAccess() {
  if (!guard()) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (auto& x : earn.devices) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = x.name;
    o["ip"] = x.ip;
    o["key"] = x.key;
  }
  sendJson(d);
}

String earnPendingPair() { return pair.active && pair.status == 0 ? pair.code : String(); }

void earnRoutes() {
  static const char* hdrs[] = {"X-Earn-Key"};
  server.collectHeaders(hdrs, 1);
  server.on("/api/earn/state", HTTP_GET, handleStationState);
  server.on("/api/earn/add", HTTP_POST, handleStationAdd);
  server.on("/api/earn/pair", HTTP_POST, handlePairRequest);
  server.on("/api/earn/pair", HTTP_GET, handlePairPoll);
  server.on("/api/earn/pair/answer", HTTP_POST, handlePairAnswer);
  server.on("/api/earn/settings", HTTP_GET, handleEarnSettingsGet);
  server.on("/api/earn/settings", HTTP_POST, handleEarnSettingsPost);
  server.on("/api/earn/device/remove", HTTP_POST, handleDeviceRemove);
  server.on("/api/earn/device/access", HTTP_GET, handleDeviceAccess);
  server.on("/api/user/wallet", HTTP_POST, handleWallet);
}
