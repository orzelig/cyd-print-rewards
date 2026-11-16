// Kindle reading from Amazon Kids (Amazon Parent Dashboard), credited exactly like reading timed here.
//
// - Fetched once an hour from 2 pm to 9 pm (household time zone, HOUSEHOLD_TZ_* in platformio.ini), each time at a random
//   moment within 5 minutes of the hour, so it isn't the same second every day.
// - Each kid's "Books" total for the week (Sunday-Saturday) is compared with what was already
//   credited; only the new minutes go to the print quota device. When the week turns over, the
//   rest of last week is picked up first (it earns print time and filament, but last week's cash
//   goal was already settled at midnight).
// - On a sign-in request or a robot check it stops completely until someone sets it up again
//   (amazon_setup.py). It never tries to get past either.
// - The sign-in values (two cookies) live only in this station's flash (NVS namespace "amazon").
//   They are never shown, printed, or sent anywhere but Amazon.
#include "station.h"
#include <Preferences.h>
#include <WiFi.h>
#include <atomic>
#include "amazon/reading_fetch.h"

namespace {

constexpr int FIRST_HOUR = 14, LAST_HOUR = 21;  // 2 pm .. 9 pm
constexpr int SUNDAY_HOUR = 6;  // and Sunday 6 am: last week's final totals (the bank keeps last week open until 8 am)
constexpr int JITTER_S = 5 * 60;
constexpr uint32_t BOOT_DELAY_MS = 30 * 1000;  // check once this long after each start...
constexpr uint32_t BOOT_MIN_GAP_S = 60 * 60;   // ...unless the last check was this recent (restart loops)

struct Kid {
  String name, id;        // Amazon's name for the child, and its directed id
  uint32_t week = 0;      // start of the week `credited` belongs to
  int32_t credited = 0;   // seconds already credited for that week
  int32_t total = -1;     // Amazon's latest total for the current week (for the status page)
};

std::vector<Kid> kids;
rr::Credentials creds;
Preferences prefs;
AmazonState state = AmazonState::Off;
String lastError;
uint32_t lastAt = 0, nextAt = 0, lastSlot = 0, pendingSlot = 0;
bool baseline = false;  // the next fetch only notes where the totals stand (nothing credited)
bool fetchNow = false;
bool bootFetch = false;  // the one check after starting is still due

// One fetch round runs in its own task so the touchscreen keeps working (TLS takes ~2 s per kid)
struct Round {
  rr::Week cur, prev;
  struct One {
    String id;
    bool wantPrev = false, done = false;
    rr::Status status = rr::NETWORK, prevStatus = rr::NETWORK;
    int32_t total = 0, prevTotal = 0;
    std::string err;
  };
  std::vector<One> kids;
};
Round* job = nullptr;
std::atomic<bool> jobDone{false};

const char* stateName(AmazonState s) {
  switch (s) {
    case AmazonState::Off: return "not set up";
    case AmazonState::Waiting: return "waiting";
    case AmazonState::Ok: return "ok";
    case AmazonState::NoMatch: return "kid not found on the station";
    case AmazonState::SignIn: return "sign-in needed";
    case AmazonState::BotCheck: return "robot check - sign in again";
    case AmazonState::Changed: return "Amazon changed its data format";
    default: return "error";
  }
}

void saveKids() {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (auto& k : kids) {
    JsonObject o = a.add<JsonObject>();
    o["n"] = k.name;
    o["id"] = k.id;
    o["w"] = k.week;
    o["c"] = k.credited;
  }
  String s;
  serializeJson(d, s);
  prefs.putString("kids", s);
}

void loadKids() {
  kids.clear();
  JsonDocument d;
  if (deserializeJson(d, prefs.getString("kids", "[]"))) return;
  for (JsonObject o : d.as<JsonArray>()) {
    Kid k;
    k.name = o["n"] | "";
    k.id = o["id"] | "";
    k.week = o["w"] | 0;
    k.credited = o["c"] | 0;
    if (k.name.length() && k.id.length()) kids.push_back(k);
  }
}

// Amazon's child name to a person on the station: same name, or same first word ("Sam Smith" -> "Sam")
Person* matchPerson(const String& amazonName) {
  String a = amazonName;
  a.trim();
  a.toLowerCase();
  int sp = a.indexOf(' ');
  String first = sp > 0 ? a.substring(0, sp) : a;
  for (auto& p : people) {
    String n = p.name;
    n.toLowerCase();
    if (n == a || n == first) return &p;
  }
  return nullptr;
}

// The next hourly slot (2 pm .. 9 pm, and Sunday 6 am) not used yet, shifted by a random -5..+5 minutes
uint32_t nextSlot(uint32_t now) {
  time_t t = now;
  struct tm lt;
  localtime_r(&t, &lt);
  for (int day = 0; day < 3; day++) {
    bool sunday = (lt.tm_wday + day) % 7 == 0;
    for (int h = sunday ? SUNDAY_HOUR : FIRST_HOUR; h <= LAST_HOUR; h++) {
      if (h > SUNDAY_HOUR && h < FIRST_HOUR) h = FIRST_HOUR;
      struct tm s = lt;
      s.tm_mday += day;
      s.tm_hour = h;
      s.tm_min = 0;
      s.tm_sec = 0;
      s.tm_isdst = -1;
      uint32_t base = mktime(&s);
      if (base <= lastSlot || base + JITTER_S < now) continue;
      if (lastAt && base < lastAt + 15 * 60) continue;  // just checked (e.g. after a restart)
      pendingSlot = base;
      int32_t jitter = (int32_t)(esp_random() % (2 * JITTER_S + 1)) - JITTER_S;
      return max<uint32_t>(base + jitter, now + 20);
    }
  }
  return 0;
}

void worker(void* arg) {
  Round* r = (Round*)arg;
  for (auto& k : r->kids) {
    if (k.wantPrev) {
      rr::KidReport rep;
      k.prevStatus = rr::fetchKidWeek(k.id.c_str(), r->prev, creds, rep, k.err);
      if (k.prevStatus == rr::OK) k.prevTotal = lround(rep.seconds);
      if (k.prevStatus == rr::SIGNED_OUT || k.prevStatus == rr::BOT_CHECK) {
        k.status = k.prevStatus;
        k.done = true;
        break;  // stop right away, never retry into a sign-in or a check
      }
    }
    rr::KidReport rep;
    k.status = rr::fetchKidWeek(k.id.c_str(), r->cur, creds, rep, k.err);
    if (k.status == rr::OK) k.total = lround(rep.seconds);
    k.done = true;
    if (k.status == rr::SIGNED_OUT || k.status == rr::BOT_CHECK) break;
  }
  jobDone = true;
  vTaskDelete(nullptr);
}

void startRound(uint32_t now) {
  if (ESP.getFreeHeap() < 70000 || ESP.getMaxAllocHeap() < 40000) {  // TLS needs ~40 KB at once
    state = AmazonState::Error;
    lastError = "Not enough free memory, will try at the next hour";
    return;
  }
  job = new Round;
  job->cur = rr::weekFor(now, 0);
  job->prev = rr::weekFor(now, 1);
  for (auto& k : kids) {
    Round::One o;
    o.id = k.id;
    o.wantPrev = !baseline && k.week && k.week != (uint32_t)job->cur.start && k.week == (uint32_t)job->prev.start;
    job->kids.push_back(o);
  }
  jobDone = false;
  if (xTaskCreatePinnedToCore(worker, "amazon", 12288, job, 1, nullptr, 1) != pdPASS) {
    delete job;
    job = nullptr;
    state = AmazonState::Error;
    lastError = "Couldn't start the fetch";
  }
}

void finishRound() {
  Round* r = job;
  job = nullptr;
  jobDone = false;
  uint32_t now = time(nullptr);
  lastAt = now;
  prefs.putUInt("last", lastAt);
  uint32_t cur = r->cur.start, prev = r->prev.start;
  AmazonState st = AmazonState::Ok;
  String err;
  bool allOk = true;
  for (size_t i = 0; i < r->kids.size() && i < kids.size(); i++) {
    Round::One& o = r->kids[i];
    Kid& k = kids[i];
    if (!o.done) continue;
    if (o.status == rr::SIGNED_OUT || o.status == rr::BOT_CHECK) {
      st = o.status == rr::SIGNED_OUT ? AmazonState::SignIn : AmazonState::BotCheck;
      err = o.err.c_str();
      allOk = false;
      break;
    }
    if (o.status != rr::OK || (o.wantPrev && o.prevStatus != rr::OK)) {  // try again next hour
      st = o.status == rr::SHAPE || o.prevStatus == rr::SHAPE ? AmazonState::Changed : AmazonState::Error;
      err = o.err.c_str();
      allOk = false;
      continue;
    }
    k.total = o.total;
    if (baseline) {  // just set up: start counting from here
      k.week = cur;
      k.credited = o.total;
      continue;
    }
    Person* p = matchPerson(k.name);
    if (!p) {
      if (st == AmazonState::Ok) st = AmazonState::NoMatch;
      allOk = false;
      continue;
    }
    if (k.week != cur) {
      // The week turned over: first credit what was still missing from last week, dated to its end
      if (k.week == prev && o.prevTotal > k.credited) {
        queueKindle(p->id, o.prevTotal - k.credited, now - (cur - 60));
        Serial.printf("kindle: %s +%lds (last week)\n", p->name.c_str(), (long)(o.prevTotal - k.credited));
      }
      k.week = cur;
      k.credited = 0;
    }
    if (o.total > k.credited) {  // Amazon totals only grow; a lower one is a correction, ignored
      queueKindle(p->id, o.total - k.credited, 0);
      Serial.printf("kindle: %s +%lds\n", p->name.c_str(), (long)(o.total - k.credited));
      k.credited = o.total;
    }
  }
  if (baseline && allOk) {
    baseline = false;
    prefs.putBool("base", false);
  }
  state = st;
  lastError = err.substring(0, 120);
  if (st == AmazonState::SignIn || st == AmazonState::BotCheck) prefs.putUChar("stop", (uint8_t)st);
  saveKids();
  delete r;
  uiDirty = true;
  Serial.printf("kindle: %s %s\n", stateName(state), lastError.c_str());
}

bool stopped() { return state == AmazonState::Off || state == AmazonState::SignIn || state == AmazonState::BotCheck; }

}  // namespace

void amazonBegin() {
  prefs.begin("amazon", false);
  creds.atMain = prefs.getString("at", "");
  creds.ubidMain = prefs.getString("ubid", "");
  loadKids();
  lastSlot = prefs.getUInt("slot", 0);
  lastAt = prefs.getUInt("last", 0);
  baseline = prefs.getBool("base", false);
  uint8_t stop = prefs.getUChar("stop", 0);
  if (creds.atMain.isEmpty() || creds.ubidMain.isEmpty() || kids.empty()) state = AmazonState::Off;
  else if (stop) state = (AmazonState)stop;
  else state = AmazonState::Waiting;
  bootFetch = state == AmazonState::Waiting;
}

void amazonLoop() {
  if (job && jobDone) finishRound();
  if (job || stopped() || WiFi.status() != WL_CONNECTED) return;
  uint32_t now = time(nullptr);
  if (now < 1700000000) return;  // clock not set yet
  if (bootFetch && millis() > BOOT_DELAY_MS) {
    bootFetch = false;
    if (now - lastAt >= BOOT_MIN_GAP_S) {
      Serial.println("kindle: checking after start");
      fetchNow = true;
    }
  }
  if (!nextAt) nextAt = nextSlot(now);
  if (!fetchNow && (!nextAt || now < nextAt)) return;
  if (!fetchNow) {
    lastSlot = pendingSlot;
    prefs.putUInt("slot", lastSlot);
  }
  fetchNow = false;
  nextAt = 0;  // the next slot is picked after this one
  startRound(now);
}

AmazonState amazonState() { return state; }

String amazonProblem() {
  switch (state) {
    case AmazonState::SignIn: return "Amazon sign-in needed";
    case AmazonState::BotCheck: return "Amazon check - sign in again";
    case AmazonState::Changed: return "Kindle data changed";
    default: return "";
  }
}

int32_t amazonWeekSeconds(int personId) {
  uint32_t now = time(nullptr);
  if (now < 1700000000 || state == AmazonState::Off) return -1;
  uint32_t cur = rr::weekFor(now, 0).start;
  for (auto& k : kids) {
    Person* p = matchPerson(k.name);
    if (p && p->id == personId && k.week == cur && !baseline) return k.credited;
  }
  return -1;
}

bool amazonSetup(const String& json, String& err) {
  if (job) {
    err = "Busy fetching, try again in a minute";
    return false;
  }
  JsonDocument d;
  if (deserializeJson(d, json)) {
    err = "Bad request";
    return false;
  }
  String at = d["at"] | "", ub = d["ubid"] | "";
  if (at.length() < 20 || at.length() > 1500 || ub.length() < 5 || ub.length() > 200) {
    err = "Sign-in values missing or the wrong size";
    return false;
  }
  std::vector<Kid> list;
  for (JsonObject o : d["kids"].as<JsonArray>()) {
    Kid k;
    k.name = o["name"] | "";
    k.id = o["id"] | "";
    if (k.name.length() && k.name.length() <= 40 && k.id.startsWith("amzn1.") && k.id.length() < 120) list.push_back(k);
  }
  if (list.empty() || list.size() > 6) {
    err = "Need 1-6 kids with an Amazon id";
    return false;
  }
  // Keep what was already credited for kids we know, so setting up again never pays twice
  for (auto& k : list)
    for (auto& old : kids)
      if (old.id == k.id) {
        k.week = old.week;
        k.credited = old.credited;
      }
  kids = list;
  creds.atMain = at;
  creds.ubidMain = ub;
  prefs.putString("at", at);
  prefs.putString("ubid", ub);
  bool fresh = true;
  for (auto& k : kids) fresh = fresh && !k.week;
  baseline = fresh && !(d["count_this_week"] | false);  // first time: count from now on
  prefs.putBool("base", baseline);
  prefs.remove("stop");
  saveKids();
  state = AmazonState::Waiting;
  lastError = "";
  fetchNow = true;
  nextAt = 0;
  uiDirty = true;
  return true;
}

void amazonForget() {
  if (job) return;
  prefs.clear();
  kids.clear();
  creds = rr::Credentials();
  state = AmazonState::Off;
  lastError = "";
  uiDirty = true;
}

void amazonFetchNow() {
  if (!stopped()) fetchNow = true;
}

void amazonStatus(JsonObject o) {
  o["state"] = stateName(state);
  if (lastError.length()) o["error"] = lastError;
  o["last"] = lastAt;
  o["next"] = nextAt;
  o["fetching"] = job != nullptr;
  o["counting_from_now"] = baseline;
  JsonArray a = o["kids"].to<JsonArray>();
  for (auto& k : kids) {
    JsonObject x = a.add<JsonObject>();
    x["name"] = k.name;
    Person* p = matchPerson(k.name);
    if (p) x["station_name"] = p->name;
    x["amazon_week_s"] = k.total;
    x["credited_s"] = k.credited;
  }
}
