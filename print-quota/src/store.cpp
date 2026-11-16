// Users, settings, persistence, quota charging and period resets.
#include "app.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>

std::vector<User> users;
Settings settings;
PrinterStatus printer;
Job job;
int claimUserId = -1;
uint32_t claimAtMs = 0;
bool resultPending = false;
bool resultOk = false;
String resultTitle, resultDetail;
int resultUserId = -1;
uint32_t resultAtMs = 0;
bool uiDirty = true;
int chargeRate = 1;
uint32_t historySeq = 0;

static const char* USERS_FILE = "/users.json";
static const char* SETTINGS_FILE = "/settings.json";
static const char* HISTORY_FILE = "/history.jsonl";
static const char* JOB_FILE = "/job.json";  // the running print, so a restart can pick it up again
static const uint32_t CLAIM_TIMEOUT_MS = 20UL * 60 * 1000;

static bool usersDirty = false;
static uint32_t lastUsersSave = 0;
static uint16_t nextId = 1;

// ---- Small helpers -------------------------------------------------------------

const char* periodName(Period p) {
  switch (p) {
    case Period::Day: return "day";
    case Period::Week: return "week";
    case Period::Month: return "month";
    default: return "none";
  }
}

Period parsePeriod(const char* s) {
  if (!s) return Period::None;
  if (!strcmp(s, "day")) return Period::Day;
  if (!strcmp(s, "week")) return Period::Week;
  if (!strcmp(s, "month")) return Period::Month;
  return Period::None;
}

const char* pstateName(PState s) {
  switch (s) {
    case PState::Idle: return "Idle";
    case PState::Prepare: return "Preparing";
    case PState::Running: return "Printing";
    case PState::Pause: return "Paused";
    case PState::Finish: return "Finished";
    case PState::Failed: return "Failed";
    default: return "Offline";
  }
}

int32_t remainingS(const User& u) {
  if (u.quotaS < 0) return INT32_MAX;
  return u.quotaS - u.usedS + u.balanceS;
}

bool canStart(const User& u) { return u.quotaS < 0 || remainingS(u) > 0; }

String fmtDur(int32_t s) {
  char buf[24];
  const char* sign = s < 0 ? "-" : "";
  uint32_t a = s < 0 ? -(int64_t)s : s;
  if (a >= 3600) snprintf(buf, sizeof buf, "%s%luh %02lum", sign, a / 3600, (a / 60) % 60);
  else snprintf(buf, sizeof buf, "%s%lum %02lus", sign, a / 60, a % 60);
  return buf;
}

String fmtDurShort(int32_t s) {
  char buf[24];
  const char* sign = s < 0 ? "-" : "";
  uint32_t a = s < 0 ? -(int64_t)s : s;
  if (a >= 3600) snprintf(buf, sizeof buf, "%s%luh %02lum", sign, a / 3600, (a / 60) % 60);
  else snprintf(buf, sizeof buf, "%s%lum", sign, a / 60);
  return buf;
}

User* findUser(int id) {
  if (id < 0) return nullptr;
  for (auto& u : users)
    if (u.id == id) return &u;
  return nullptr;
}

// ---- Time & periods ----------------------------------------------------------------

bool timeValid() { return time(nullptr) > 1700000000; }
uint32_t nowEpoch() { return time(nullptr); }

static time_t periodStartFor(Period p, time_t now) {
  struct tm t;
  localtime_r(&now, &t);
  t.tm_hour = t.tm_min = t.tm_sec = 0;
  if (p == Period::Week) t.tm_mday -= (t.tm_wday + 6) % 7;  // back to Monday
  else if (p == Period::Month) t.tm_mday = 1;
  t.tm_isdst = -1;
  return mktime(&t);
}

uint32_t nextResetEpoch(const User& u) {
  if (u.period == Period::None || !timeValid()) return 0;
  time_t start = periodStartFor(u.period, time(nullptr));
  struct tm t;
  localtime_r(&start, &t);
  if (u.period == Period::Day) t.tm_mday += 1;
  else if (u.period == Period::Week) t.tm_mday += 7;
  else t.tm_mon += 1;
  t.tm_isdst = -1;
  return mktime(&t);
}

void anchorPeriod(User& u) {
  u.periodStart = (u.period != Period::None && timeValid()) ? periodStartFor(u.period, time(nullptr)) : 0;
}

static void checkPeriods() {
  if (!timeValid()) return;
  time_t now = time(nullptr);
  for (auto& u : users) {
    if (u.period == Period::None) continue;
    uint32_t ps = periodStartFor(u.period, now);
    if (ps == u.periodStart) continue;
    if (u.periodStart != 0) {
      Serial.printf("new %s for %s: usage reset\n", periodName(u.period), u.name.c_str());
      u.usedS = 0;
    }
    u.periodStart = ps;
    usersDirty = true;
    uiDirty = true;
  }
}

// ---- Persistence -------------------------------------------------------------------

static void writeJsonFile(const char* path, JsonDocument& doc) {
  String tmp = String(path) + ".tmp";
  File f = LittleFS.open(tmp, "w");
  if (!f) return;
  serializeJson(doc, f);
  f.close();
  LittleFS.remove(path);
  LittleFS.rename(tmp, path);
}

static void loadUsers() {
  File f = LittleFS.open(USERS_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  for (JsonObject o : doc.as<JsonArray>()) {
    User u;
    u.id = o["id"] | 0;
    u.name = o["name"] | "";
    u.quotaS = o["q"] | 7200;
    u.period = parsePeriod(o["p"] | "week");
    u.usedS = o["u"] | 0;
    u.balanceS = o["b"] | 0;
    u.periodStart = o["ps"] | 0;
    u.totalS = o["t"] | 0;
    u.filC = o["fc"] | 0;
    u.cashC = o["cc"] | 0;
    u.filFrac = o["ff"] | 0;
    u.weekReadS = o["wr"] | 0;
    u.weekPaidC = o["wp"] | 0;
    u.prevReadS = o["pr"] | 0;
    u.prevPaidC = o["pp"] | 0;
    u.readTotalS = o["rt"] | 0;
    u.choreDay = o["cd"] | 0;
    u.earns = o["ea"] | true;
    for (JsonPair kv : o["ch"].as<JsonObject>()) u.choreCount[atoi(kv.key().c_str())] = kv.value().as<uint8_t>();
    if (u.id == 0 || u.name.isEmpty()) continue;
    users.push_back(u);
    nextId = max<uint16_t>(nextId, u.id + 1);
  }
}

// Saved together with the users, so balances and the job's charged time always match
static void saveJob() {
  if (!job.active) {
    LittleFS.remove(JOB_FILE);
    return;
  }
  JsonDocument d;
  d["user"] = job.userId;
  d["name"] = job.name;
  d["start"] = job.startedEpoch;
  d["charged"] = job.chargedS;
  d["alarmed"] = job.alarmed;
  d["warned"] = job.warned;
  d["pct"] = printer.percent;
  d["saved"] = timeValid() ? nowEpoch() : 0;
  writeJsonFile(JOB_FILE, d);
}

static uint32_t resumeSaved = 0, resumeAtMs = 0;  // off-time still to charge once the clock is set

// Charge the time between the last save before the restart and the moment we resumed
static void chargeRestartGap() {
  if (!resumeSaved || !timeValid() || !job.active) return;
  uint32_t resumedEpoch = nowEpoch() - (millis() - resumeAtMs) / 1000;
  int32_t gap = resumedEpoch > resumeSaved ? min<uint32_t>(resumedEpoch - resumeSaved, 4 * 3600) : 0;
  resumeSaved = 0;
  job.chargedS += gap;
  if (User* u = findUser(job.userId)) chargeUser(*u, gap);
  Serial.printf("charged %ld s printed while restarting\n", (long)gap);
  usersChanged();
}

// After a restart mid-print: continue the same job for the same person, and charge the
// minutes the device was off (the printer kept printing).
static void resumeJob() {
  File f = LittleFS.open(JOB_FILE, "r");
  if (!f) return;
  JsonDocument d;
  DeserializationError err = deserializeJson(d, f);
  f.close();
  if (err) return;
  String name = d["name"] | "";
  int pct = d["pct"] | 0;
  uint32_t saved = d["saved"] | 0;
  bool sameJob = (name == job.name || job.name.isEmpty() || name.isEmpty()) && printer.percent + 2 >= pct;
  bool recent = !saved || !timeValid() || nowEpoch() - saved < 12 * 3600;
  if (!sameJob || !recent) return;
  job.userId = findUser(d["user"] | -1) ? (int)(d["user"] | -1) : -1;
  job.chargedS = d["charged"] | 0;
  job.startedEpoch = d["start"] | job.startedEpoch;
  job.alarmed = d["alarmed"] | false;
  job.warned = d["warned"] | false;
  if (job.name.isEmpty()) job.name = name;
  Serial.printf("resumed job %s for user %d\n", job.name.c_str(), job.userId);
  resumeSaved = saved;
  resumeAtMs = millis();
  chargeRestartGap();  // now, or as soon as the clock is set
}

void saveUsers() {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  for (auto& u : users) {
    JsonObject o = a.add<JsonObject>();
    o["id"] = u.id;
    o["name"] = u.name;
    o["q"] = u.quotaS;
    o["p"] = periodName(u.period);
    o["u"] = u.usedS;
    o["b"] = u.balanceS;
    o["ps"] = u.periodStart;
    o["t"] = u.totalS;
    o["fc"] = u.filC;
    o["cc"] = u.cashC;
    o["ff"] = u.filFrac;
    o["wr"] = u.weekReadS;
    o["wp"] = u.weekPaidC;
    o["pr"] = u.prevReadS;
    o["pp"] = u.prevPaidC;
    o["rt"] = u.readTotalS;
    o["cd"] = u.choreDay;
    o["ea"] = u.earns;
    JsonObject ch = o["ch"].to<JsonObject>();
    for (auto& kv : u.choreCount) ch[String(kv.first)] = kv.second;
  }
  writeJsonFile(USERS_FILE, doc);
  saveJob();
  usersDirty = false;
  lastUsersSave = millis();
}

static void loadSettings() {
  File f = LittleFS.open(SETTINGS_FILE, "r");
  if (!f) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;
  settings.printerIp = doc["printer_ip"] | "";
  settings.serial = doc["serial"] | "";
  settings.accessCode = doc["access_code"] | "";
  settings.demo = doc["demo"] | false;
  settings.graceMin = doc["grace_min"] | 5;
  settings.refundFailed = doc["refund_failed"] | false;
  settings.tz = doc["tz"] | "UTC0";
  settings.adminPass = doc["admin_pass"] | "";
}

void saveSettings() {
  JsonDocument doc;
  doc["printer_ip"] = settings.printerIp;
  doc["serial"] = settings.serial;
  doc["access_code"] = settings.accessCode;
  doc["demo"] = settings.demo;
  doc["grace_min"] = settings.graceMin;
  doc["refund_failed"] = settings.refundFailed;
  doc["tz"] = settings.tz;
  doc["admin_pass"] = settings.adminPass;
  writeJsonFile(SETTINGS_FILE, doc);
}

void storeBegin() {
  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  loadSettings();
  loadUsers();
  earnBegin();
  Serial.printf("loaded %u users\n", (unsigned)users.size());
}

void usersChanged() {
  usersDirty = true;
  lastUsersSave = 0;  // save on the next tick
  uiDirty = true;
}

User* addUser(const String& name, int32_t quotaS, Period p) {
  if (users.size() >= MAX_USERS) return nullptr;
  User u;
  u.id = nextId++;
  u.name = name;
  u.quotaS = quotaS;
  u.period = p;
  anchorPeriod(u);
  users.push_back(u);
  usersChanged();
  return &users.back();
}

bool deleteUser(int id) {
  for (size_t i = 0; i < users.size(); i++) {
    if (users[i].id != id) continue;
    users.erase(users.begin() + i);
    if (job.userId == id) job.userId = -1;
    if (claimUserId == id) claimUserId = -1;
    usersChanged();
    return true;
  }
  return false;
}

// ---- History -------------------------------------------------------------------------

// The history file is read one line at a time so it never has to fit in memory.
static size_t countHistoryLines() {
  File f = LittleFS.open(HISTORY_FILE, "r");
  if (!f) return 0;
  size_t n = 0;
  uint8_t buf[256];
  while (int len = f.read(buf, sizeof buf))
    for (int i = 0; i < len; i++) n += buf[i] == '\n';
  f.close();
  return n;
}

void forEachHistoryLine(size_t newest, std::function<void(const String&)> fn) {
  size_t skip = countHistoryLines();
  skip = skip > newest ? skip - newest : 0;
  File f = LittleFS.open(HISTORY_FILE, "r");
  if (!f) return;
  for (size_t i = 0; f.available(); i++) {
    String l = f.readStringUntil('\n');
    if (i < skip) continue;
    l.trim();
    if (l.length()) fn(l);
  }
  f.close();
}

// Keep only the newest `keep` lines, copying through a temporary file
static void trimHistory(size_t keep) {
  size_t skip = countHistoryLines();
  if (skip <= keep) return;
  skip -= keep;
  File in = LittleFS.open(HISTORY_FILE, "r");
  File out = LittleFS.open("/history.tmp", "w");
  if (!in || !out) return;
  uint8_t buf[256];
  while (int len = in.read(buf, sizeof buf)) {
    int i = 0;
    while (skip && i < len)
      if (buf[i++] == '\n') skip--;
    if (i < len) out.write(buf + i, len - i);
  }
  in.close();
  out.close();
  LittleFS.remove(HISTORY_FILE);
  LittleFS.rename("/history.tmp", HISTORY_FILE);
}

void logHistory(JsonDocument& e) {
  File f = LittleFS.open(HISTORY_FILE, "a");
  if (!f) return;
  serializeJson(e, f);
  f.print('\n');
  size_t size = f.size();
  f.close();
  if (size > 48000) trimHistory(120);  // keep the newest entries
  historySeq++;
}

void logTopup(const User& u, int32_t s, const String& note) {
  JsonDocument e;
  e["type"] = "topup";
  e["t"] = nowEpoch();
  e["user"] = u.name;
  e["s"] = s;
  e["note"] = note;
  e["left"] = u.quotaS < 0 ? 0 : remainingS(u);
  logHistory(e);
}

void clearHistory() {
  LittleFS.remove(HISTORY_FILE);
  historySeq++;
}

// ---- Charging -----------------------------------------------------------------------

void chargeUser(User& u, int32_t s) {
  if (s <= 0) return;
  u.totalS += s;
  if (u.quotaS < 0) {
    u.usedS += s;
  } else {
    int32_t fromAllowance = min(s, max<int32_t>(0, u.quotaS - u.usedS));
    u.usedS += fromAllowance;
    int32_t rest = s - fromAllowance;
    int32_t fromBalance = min(rest, max<int32_t>(0, u.balanceS));
    u.balanceS -= fromBalance;
    u.usedS += rest - fromBalance;  // anything left is overdraft
  }
  usersDirty = true;
}

void refundUser(User& u, int32_t s) {
  if (s <= 0) return;
  u.totalS -= min<uint32_t>(s, u.totalS);
  int32_t back = min(s, u.usedS);
  u.usedS -= back;
  u.balanceS += s - back;
  usersDirty = true;
}

void assignJob(int userId) {
  if (!job.active) return;
  if (User* old = findUser(job.userId)) refundUser(*old, job.chargedS);
  User* nu = findUser(userId);
  if (nu) chargeUser(*nu, job.chargedS);
  job.userId = nu ? userId : -1;
  job.alarmed = job.warned = false;
  Serial.printf("job assigned to %s\n", nu ? nu->name.c_str() : "nobody");
  usersChanged();
  saveUsers();
}

static void startJob() {
  job = Job();
  resumeSaved = 0;
  job.active = true;
  job.startedEpoch = nowEpoch();
  job.name = printer.job;
  User* claimed = findUser(claimUserId);
  if (claimed && canStart(*claimed)) job.userId = claimUserId;  // only someone with time left
  claimUserId = -1;
  if (job.userId < 0 && printer.state != PState::Prepare) resumeJob();  // joined mid-print: probably a restart
  saveUsers();
  Serial.printf("job started: %s for user %d\n", job.name.c_str(), job.userId);
  uiDirty = true;
}

static void endJob(PState how) {
  bool ok = how == PState::Finish;
  User* u = findUser(job.userId);
  bool refunded = false;
  if (u && !ok && (job.chargedS <= (int32_t)settings.graceMin * 60 || settings.refundFailed)) {
    refundUser(*u, job.chargedS);
    refunded = true;
  }

  JsonDocument e;
  e["type"] = "print";
  e["t"] = job.startedEpoch ? job.startedEpoch : nowEpoch();
  e["end"] = nowEpoch();
  e["user"] = u ? u->name : String("(nobody)");
  e["job"] = job.name;
  e["s"] = job.chargedS;
  e["result"] = ok ? "finished" : (how == PState::Failed ? "failed" : "cancelled");
  e["refunded"] = refunded;
  logHistory(e);

  resultOk = ok;
  resultUserId = u ? u->id : -1;
  if (ok) resultTitle = "Print finished";
  else resultTitle = refunded ? "Stopped - refunded" : "Print failed";
  if (!u) resultDetail = "Nobody was charged (" + fmtDurShort(job.chargedS) + ")";
  else if (refunded) resultDetail = "No time charged to " + u->name;
  else resultDetail = "Charged " + fmtDurShort(job.chargedS) + " to " + u->name;
  resultPending = true;
  resultAtMs = millis();

  Serial.printf("job ended (%s): %ld s, user %d, refunded %d\n", pstateName(how), (long)job.chargedS,
                job.userId, refunded);
  job = Job();
  usersChanged();
  saveUsers();
}

void onPrinterState(PState prev, PState now) {
  Serial.printf("printer: %s -> %s\n", pstateName(prev), pstateName(now));
  bool printing = now == PState::Prepare || now == PState::Running || now == PState::Pause;
  if (!job.active && printing) startJob();
  else if (job.active && (now == PState::Finish || now == PState::Failed || now == PState::Idle))
    endJob(now);
  uiDirty = true;
}

void tickQuota() {
  static uint32_t lastSec = millis(), lastPeriodCheck = 0;

  while (millis() - lastSec >= 1000) {  // catches up if the loop was blocked
    lastSec += 1000;
    if (!job.active || printer.state != PState::Running) continue;
    job.chargedS += chargeRate;
    User* u = findUser(job.userId);
    if (!u) continue;
    chargeUser(*u, chargeRate);
    if (u->quotaS < 0) continue;
    int32_t left = remainingS(*u);
    if (!job.warned && left <= 600 && left > 0) job.warned = true;
    if (!job.alarmed && left <= 0) {  // warn only: the print always runs to the end
      job.alarmed = true;
      Serial.printf("%s is out of time; print continues, balance goes negative\n", u->name.c_str());
    }
  }

  if (job.active) {
    chargeRestartGap();
    if (job.name.isEmpty() && printer.job.length()) job.name = printer.job;
    if (User* u = findUser(job.userId))
      if (job.alarmed && remainingS(*u) > 0) job.alarmed = false;  // topped up
  }

  if (claimUserId >= 0) {
    User* c = findUser(claimUserId);
    if (!c || !canStart(*c) || millis() - claimAtMs > CLAIM_TIMEOUT_MS) {
      claimUserId = -1;
      uiDirty = true;
    }
  }

  if (millis() - lastPeriodCheck > 10000) {
    lastPeriodCheck = millis();
    checkPeriods();
    earnTick();
  }

  // Save edits quickly; while printing, charges are saved once a minute
  uint32_t saveAfter = (job.active && lastUsersSave != 0) ? 60000 : 1000;
  if (usersDirty && millis() - lastUsersSave > saveAfter) saveUsers();
}
