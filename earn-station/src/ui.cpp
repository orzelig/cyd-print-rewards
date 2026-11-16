// Touchscreen UI: pick a person, start/stop reading, record chores.
// Big finger-sized buttons; each person has their own color (same as on the print quota device).
#include "station.h"
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Wire.h>

constexpr int XPT_IRQ = 36, XPT_MOSI = 32, XPT_MISO = 39, XPT_CLK = 25, XPT_CS = 33;
constexpr int TOUCH_X_MIN = 200, TOUCH_X_MAX = 3700;
constexpr int TOUCH_Y_MIN = 240, TOUCH_Y_MAX = 3800;
constexpr int LED_R = 4, LED_G = 16, LED_B = 17;  // active LOW
constexpr int SPEAKER = 26;

static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen ts(XPT_CS, XPT_IRQ);
static TFT_eSPI tft;
#include "ui_kit.h"
using namespace kit;

// ---- Layout ---------------------------------------------------------------------------------

const Rect R_BANNER{6, 30, 308, 52};
const Rect R_BACK{216, 37, 90, 38};
const Rect R_READ{6, 190, 151, 46};
const Rect R_CHORES{163, 190, 151, 46};
const Rect R_STOP{6, 192, 308, 44};
const Rect R_YES{6, 124, 308, 58};
const Rect R_NO{6, 190, 308, 46};
const Rect R_ASK_YES{6, 98, 308, 64};
const Rect R_ASK_NO{6, 172, 308, 60};
const Rect R_UP{258, 88, 56, 68};
const Rect R_DOWN{258, 160, 56, 68};
constexpr int CHORES_PER_PAGE = 3;

enum class Screen { None, Connect, Home, Person, Reading, Chores, Confirm, Ask, Toast };
static Screen screen = Screen::None, nav = Screen::Home;
static int curUser = -1, curChore = -1, askUser = -1;
static int page = 0, chorePage = 0;
static uint32_t lastTouchMs = 0;
bool uiDirty = true;

static String toastTitle, toastDetail;
static bool toastGood = true;
static uint32_t toastUntil = 0;

// Preview: show a screen with made-up reading data for a while, changing nothing (debug API)
static Screen previewScreen = Screen::None;
static uint32_t previewUntil = 0;
static Session previewSession;
static bool previewing() { return previewUntil && millis() < previewUntil; }

static Session* sess(int userId) {
  if (previewing() && userId == previewSession.userId &&
      (previewScreen == Screen::Reading || previewScreen == Screen::Ask))
    return &previewSession;
  return findSession(userId);
}

// ---- Helpers -------------------------------------------------------------------------------

static void click() { tone(SPEAKER, 3200, 25); }

static String clockText(int32_t s) {
  char buf[16];
  if (s >= 3600) snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", (long)s / 3600, (long)(s / 60) % 60, (long)s % 60);
  else snprintf(buf, sizeof buf, "%02ld:%02ld", (long)s / 60, (long)s % 60);
  return buf;
}

static int32_t unpostedS(int userId) {
  Session* s = sess(userId);
  return s ? sessionSeconds(*s) - s->postedS : 0;
}

static int32_t weekRead(const Person& p) { return p.weekReadS + unpostedS(p.id); }
static int32_t weekFrac(const Person& p) { return rules.goalS > 0 ? (int64_t)weekRead(p) * 1000 / rules.goalS : 1000; }

static String leftText(const Person& p) { return p.unlimited ? String("No limit") : fmtDur(p.leftS); }
static uint16_t leftColor(const Person& p) {
  return p.unlimited ? GOOD : p.leftS <= 0 ? BAD : p.leftS <= 1800 ? WARN : GOOD;
}

static String weekLeftText() {
  int32_t s = weekSecondsLeft();
  if (s < 0) return "";
  if (s >= 2 * 86400) return String(s / 86400) + " days left";
  return fmtDur(s) + " left";
}

// One line about the weekly cash goal
static String goalLine(const Person& p, uint16_t& color) {
  int32_t w = weekRead(p);
  if (rules.goalS <= 0 || rules.cashRate == 0) {
    color = FAINT;
    return "";
  }
  int32_t cash = (int64_t)w * rules.cashRate / 360000;
  if (w >= rules.goalS) {
    color = GOOD;
    return "Goal reached! " + money(cash) + " cash earned";
  }
  color = WARN;
  String s = money(cash) + " cash waiting - " + fmtDur(rules.goalS - w) + " to go";
  String wl = weekLeftText();
  return wl.length() && textW(s + ", " + wl, SM) <= 304 ? s + ", " + wl : s;
}

static String earnedText(int32_t sec) {
  return "+" + fmtDur((int64_t)sec * rules.printPct / 100) + " print, +" + money((int64_t)sec * rules.filRate / 360000) +
         " filament";
}

static String rewardText(const ChoreDef& c, bool longForm) {
  String s;
  auto add = [&](const String& part) { s += (s.length() ? "  " : "") + part; };
  if (c.printS) add("+" + fmtDur(c.printS) + (longForm ? " print time" : ""));
  if (c.filC) add("+" + money(c.filC) + (longForm ? " filament" : ""));
  if (c.cashC) add("+" + money(c.cashC) + (longForm ? " cash" : ""));
  return s.length() ? s : String("no reward");
}

static bool choreDone(const Person& p, const ChoreDef& c) {
  if (c.perDay <= 0) return false;
  auto it = p.choresToday.find(c.id);
  return it != p.choresToday.end() && it->second >= c.perDay;
}

void uiToast(const String& title, const String& detail, bool good) {
  toastTitle = title;
  toastDetail = detail;
  toastGood = good;
  toastUntil = millis() + 3500;
  uiDirty = true;
}

void uiMessage(const String& title, const String& detail) {
  tft.fillScreen(BG);
  text(title, 160, 100, LG, WARN, BG, MC_DATUM);
  text(detail, 160, 138, SM, MUTED, BG, MC_DATUM);
}

// ---- Status bar -------------------------------------------------------------------

static String shownL, shownR;

static void drawStatus(bool force) {
  String l = "Earning station";
  String problem = amazonProblem();
  if (problem.length()) l = problem;
  if (!sessions.empty()) {
    l = "Reading:";
    for (auto& s : sessions)
      if (Person* p = findPerson(s.userId)) l += " " + p->name;
  }
  String r;
  uint16_t dc = GOOD;
  switch (bankState) {
    case BankState::Ok:
      r = queuedCount() ? "Sending..." : "Connected";
      dc = queuedCount() ? SKY : GOOD;
      break;
    case BankState::Offline:
      r = queuedCount() ? "Offline - " + String(queuedCount()) + " saved" : String("Offline");
      dc = WARN;
      break;
    case BankState::Pairing: r = "Waiting for OK"; dc = SKY; break;
    case BankState::Denied: r = "Not allowed"; dc = BAD; break;
    case BankState::Finding: r = "Searching..."; dc = WARN; break;
    default: r = wifiPortalActive() ? "Wi-Fi setup" : "No Wi-Fi"; dc = WARN; break;
  }
  if (!force && l == shownL && r == shownR) return;
  shownL = l;
  shownR = r;
  tft.fillRect(0, 0, 320, 26, BG);
  dot(12, 13, 5, dc, BG);
  text(r, 314, 13, SM, FAINT, BG, MR_DATUM);
  uint16_t lc = !sessions.empty() ? GOOD : problem.length() ? WARN : MUTED;
  text(fit(l, 320 - textW(r, SM) - 44, SM), 24, 13, SM, lc, BG, ML_DATUM);
}

// Colored strip with the person's avatar and name, and a Back button
static void banner(const Person& p, const String& line2) {
  uint16_t c = personColor(p.id);
  card(R_BANNER, c);
  avatar(R_BANNER.x + 28, R_BANNER.cy(), 18, c, p.name, true, c);
  int tw = R_BACK.x - 66;
  if (line2.isEmpty()) {
    text(fit(p.name, tw, LG), 58, R_BANNER.cy() + 1, LG, onColor(c), c, ML_DATUM);
  } else {
    text(fit(p.name, tw, MD), 58, R_BANNER.cy() - 9, MD, onColor(c), c, ML_DATUM);
    text(fit(line2, tw, SM), 58, R_BANNER.cy() + 13, SM, softOn(c), c, ML_DATUM);
  }
  button(R_BACK, "Back", mix(INK, c, 90), MD, c);
}

// ---- Connect (before there's anything to show) -----------------------------------------

static void lines(std::initializer_list<String> ls, int y, uint16_t color) {
  for (auto& l : ls) {
    text(fit(l, 308, SM), 160, y, SM, color, BG, MC_DATUM);
    y += 22;
  }
}

static void drawConnect() {
  tft.fillScreen(BG);
  drawStatus(true);
  text("Earning station", 160, 60, LG, TEXT, BG, MC_DATUM);
  switch (bankState) {
    case BankState::NoWifi:
      if (wifiPortalActive())
        lines({"On your phone, join the Wi-Fi network", "EarnStation-Setup", "and pick your home Wi-Fi."}, 104, MUTED);
      else lines({"Connecting to Wi-Fi..."}, 110, MUTED);
      break;
    case BankState::Pairing:
      lines({"Open printquota.local and tap Allow.", "Check that it shows this code:"}, 100, MUTED);
      text(pairCode, 160, 172, XL, WARN, BG, MC_DATUM);
      break;
    case BankState::Denied: lines({"The connection was denied.", "Tap the screen to ask again."}, 110, WARN); break;
    case BankState::Ok: lines({"Nobody to show yet.", "Add people at printquota.local"}, 110, MUTED); break;
    default:
      lines({bankError.length() ? bankError : String("Looking for printquota.local..."),
             "The print quota device must be on the same Wi-Fi."},
            110, MUTED);
      break;
  }
}

// ---- Home: who's earning? -------------------------------------------------------------
// Up to 3 people: one wide row each. More: a 2 x 3 grid, the 6th tile turns the page.

static bool wide() { return people.size() <= 3; }
static int perPage() { return wide() ? 3 : people.size() > 6 ? 5 : 6; }
static int pageCount() { return max(1, (int)((people.size() + perPage() - 1) / perPage())); }
static Rect tileRect(int i) {
  if (wide()) return {6, int16_t(62 + i * 59), 308, 54};
  return {int16_t(6 + (i % 2) * 157), int16_t(62 + (i / 2) * 59), 151, 54};
}

static void tileLine2(int i, const Person& p) {
  Rect r = tileRect(i);
  uint16_t c = personColor(p.id);
  Session* s = sess(p.id);
  int x = r.x + 52, y = r.y + 39;
  if (wide()) {
    String wk = fmtDur(weekRead(p)) + " / " + fmtDur(rules.goalS);
    if (s) {
      text("Reading " + clockText(sessionSeconds(*s)), x, y, SM, onColor(c), c, ML_DATUM, r.w - 64);
    } else {
      tft.fillRect(x, y - 6, r.w - 64, 16, c);
      int bw = constrain(r.x + r.w - 14 - textW(wk, SM) - 12 - x, 40, 130);  // leave room for the text
      bar(x, y - 4, bw, 9, weekFrac(p), TEXT, mix(INK, c, 80), c);
      text(wk, r.x + r.w - 14, y, SM, softOn(c), c, MR_DATUM);
    }
  } else {
    String line = s ? "Reading " + clockText(sessionSeconds(*s)) : fmtDur(weekRead(p)) + " this week";
    text(fit(line, r.w - 58, SM), x - 6, y, SM, s ? onColor(c) : softOn(c), c, ML_DATUM, r.w - 54);
  }
}

static void drawTile(int i, const Person& p) {
  Rect r = tileRect(i);
  uint16_t c = personColor(p.id);
  card(r, c);
  avatar(r.x + 26, r.cy(), 16, c, p.name, true, c);
  int x = wide() ? r.x + 52 : r.x + 46;
  if (wide()) text(leftText(p) + " print", r.x + r.w - 14, r.y + 18, SM, softOn(c), c, MR_DATUM);
  int nameW = wide() ? r.w - 70 - textW(leftText(p) + " print", SM) : r.w - 52;
  text(fit(p.name, nameW, MD), x, r.y + 17, MD, onColor(c), c, ML_DATUM);
  tileLine2(i, p);
}

static void drawHome() {
  tft.fillScreen(BG);
  drawStatus(true);
  text("Who's earning?", 8, 44, MD, TEXT, BG, ML_DATUM);
  page = constrain(page, 0, pageCount() - 1);
  int n = perPage();
  for (int i = 0; i < n; i++) {
    size_t idx = page * n + i;
    if (idx < people.size()) drawTile(i, people[idx]);
  }
  if (!wide() && pageCount() > 1) {
    Rect r = tileRect(5);
    card(r, CARD2);
    text("More", r.x + 16, r.cy() - 9, MD, TEXT, CARD2, ML_DATUM);
    text(String(page + 1) + " of " + String(pageCount()), r.x + 16, r.cy() + 13, SM, MUTED, CARD2, ML_DATUM);
    chevron(r.x + r.w - 24, r.cy(), 10, false, TEXT, CARD2);
  }
}

static void homeDynamic() {
  int n = perPage();
  for (int i = 0; i < n; i++) {
    size_t idx = page * n + i;
    if (idx < people.size() && sess(people[idx].id)) tileLine2(i, people[idx]);
  }
}

static void tapHome(int x, int y) {
  if (!wide() && pageCount() > 1 && tileRect(5).has(x, y)) {
    click();
    page = (page + 1) % pageCount();
    uiDirty = true;
    return;
  }
  int n = perPage();
  for (int i = 0; i < n; i++) {
    size_t idx = page * n + i;
    if (idx >= people.size() || !tileRect(i).has(x, y)) continue;
    click();
    curUser = people[idx].id;
    nav = findSession(curUser) ? Screen::Reading : Screen::Person;
    uiDirty = true;
    return;
  }
}

// ---- Person ------------------------------------------------------------------------------

static void statBox(int x, const String& label, const String& value, uint16_t color) {
  Rect r{int16_t(x), 86, 100, 46};
  card(r, CARD);
  text(label, r.cx(), r.y + 12, SM, MUTED, CARD, MC_DATUM);
  Font f = fitFont(value, 92, MD, SM);  // e.g. "11h 18m" is too wide in the big font
  text(fit(value, 92, f), r.cx(), r.y + 32, f, color, CARD, MC_DATUM);
}

static void weekBlock(const Person& p, int y, bool withGoal) {
  int32_t w = weekRead(p);
  text("This week", 8, y, SM, MUTED, BG, ML_DATUM, 144);  // includes Kindle reading
  text(fmtDur(w) + " of " + fmtDur(rules.goalS), 312, y, SM, TEXT, BG, MR_DATUM, 150);
  bool reached = rules.goalS > 0 && w >= rules.goalS;
  bar(8, y + 11, 304, 10, weekFrac(p), reached ? GOOD : SKY, CARD2, BG);
  if (!withGoal) return;
  uint16_t col;
  String line = goalLine(p, col);
  text(fit(line, 304, SM), 160, y + 33, SM, col, BG, MC_DATUM, 308);
}

static void drawPerson() {
  Person* p = findPerson(curUser);
  if (!p) {
    nav = Screen::Home;
    uiDirty = true;
    return;
  }
  tft.fillScreen(BG);
  drawStatus(true);
  banner(*p, "");
  statBox(6, "Print time", leftText(*p), leftColor(*p));
  statBox(110, "Filament", money(p->filC), TEXT);
  statBox(214, "Cash", money(p->cashC), TEXT);
  weekBlock(*p, 144, true);
  button(R_READ, sess(p->id) ? "Reading..." : "Read", GO);
  button(R_CHORES, "Chores", ACCENT);
}

static void tapPerson(int x, int y) {
  if (R_BACK.has(x, y)) {
    click();
    nav = Screen::Home;
  } else if (R_READ.has(x, y)) {
    click();
    startReading(curUser);
    nav = Screen::Reading;
  } else if (R_CHORES.has(x, y)) {
    click();
    chorePage = 0;
    nav = Screen::Chores;
  } else {
    return;
  }
  uiDirty = true;
}

// ---- Reading -------------------------------------------------------------------------------

static int32_t readingWeekShown = -1;

static void readingDynamic() {
  Person* p = findPerson(curUser);
  Session* s = sess(curUser);
  if (!p || !s) return;
  int32_t sec = sessionSeconds(*s);
  text(clockText(sec), 160, 113, XL, TEXT, BG, MC_DATUM, 300);
  text(fit(earnedText(sec), 304, SM), 160, 147, SM, GOOD, BG, MC_DATUM, 308);
  int32_t w = weekRead(*p);
  if (w / 60 != readingWeekShown) {  // the bar moves once a minute at most
    readingWeekShown = w / 60;
    weekBlock(*p, 166, false);
  }
}

static void drawReading() {
  Person* p = findPerson(curUser);
  if (!p || !sess(curUser)) {
    nav = p ? Screen::Person : Screen::Home;
    uiDirty = true;
    return;
  }
  tft.fillScreen(BG);
  drawStatus(true);
  banner(*p, "is reading");
  button(R_STOP, "Stop reading", STOP);
  readingWeekShown = -1;
  readingDynamic();
}

static void tapReading(int x, int y) {
  if (R_BACK.has(x, y)) {
    click();
    nav = Screen::Home;  // the session keeps running
    uiDirty = true;
  } else if (R_STOP.has(x, y)) {
    click();
    Session* s = findSession(curUser);
    Person* p = findPerson(curUser);
    if (s && p) {
      int32_t sec = sessionSeconds(*s);
      stopReading(curUser, true);
      uiToast("Nice reading, " + p->name + "!", fmtDur(sec) + " read: " + earnedText(sec), true);
    }
    nav = Screen::Person;
    uiDirty = true;
  }
}

// ---- Chores ---------------------------------------------------------------------------------

static int chorePageCount() { return max(1, (int)((chores.size() + CHORES_PER_PAGE - 1) / CHORES_PER_PAGE)); }
static Rect choreRect(int i) {
  return {6, int16_t(88 + i * 48), int16_t(chorePageCount() > 1 ? 246 : 308), 44};
}

static void drawChores() {
  Person* p = findPerson(curUser);
  if (!p) {
    nav = Screen::Home;
    uiDirty = true;
    return;
  }
  tft.fillScreen(BG);
  drawStatus(true);
  banner(*p, "Pick a chore");
  if (chores.empty()) return lines({"No chores set up yet.", "Add them at printquota.local"}, 140, MUTED);
  chorePage = constrain(chorePage, 0, chorePageCount() - 1);
  for (int i = 0; i < CHORES_PER_PAGE; i++) {
    size_t idx = chorePage * CHORES_PER_PAGE + i;
    if (idx >= chores.size()) break;
    const ChoreDef& c = chores[idx];
    bool done = choreDone(*p, c);
    Rect r = choreRect(i);
    uint16_t bg = done ? mix(CARD, BG, 120) : CARD;
    card(r, bg);
    int tw = r.w - (done ? 52 : 26);
    text(fit(c.name, tw, MD), r.x + 14, r.y + 14, MD, done ? FAINT : TEXT, bg, ML_DATUM);
    text(fit(done ? String("Done today") : rewardText(c, false), tw, SM), r.x + 14, r.y + 33, SM, done ? FAINT : GOOD, bg,
         ML_DATUM);
    if (done) checkIcon(r.x + r.w - 24, r.cy(), 8, GOOD, bg);
  }
  if (chorePageCount() > 1) {
    card(R_UP, chorePage > 0 ? CARD2 : CARD);
    arrow(R_UP, true, chorePage > 0 ? TEXT : FAINT, CARD2);
    card(R_DOWN, chorePage < chorePageCount() - 1 ? CARD2 : CARD);
    arrow(R_DOWN, false, chorePage < chorePageCount() - 1 ? TEXT : FAINT, CARD2);
  }
}

static void tapChores(int x, int y) {
  if (R_BACK.has(x, y)) {
    click();
    nav = Screen::Person;
    uiDirty = true;
    return;
  }
  if (chorePageCount() > 1 && (R_UP.has(x, y) || R_DOWN.has(x, y))) {
    int next = constrain(chorePage + (R_UP.has(x, y) ? -1 : 1), 0, chorePageCount() - 1);
    if (next != chorePage) {
      click();
      chorePage = next;
      uiDirty = true;
    }
    return;
  }
  Person* p = findPerson(curUser);
  for (int i = 0; i < CHORES_PER_PAGE; i++) {
    size_t idx = chorePage * CHORES_PER_PAGE + i;
    if (idx >= chores.size() || !choreRect(i).has(x, y) || !p) continue;
    if (choreDone(*p, chores[idx])) {
      tone(SPEAKER, 400, 200);
      return;
    }
    click();
    curChore = chores[idx].id;
    nav = Screen::Confirm;
    uiDirty = true;
    return;
  }
}

static void drawConfirm() {
  Person* p = findPerson(curUser);
  ChoreDef* c = findChore(curChore);
  if (!p || !c) {
    nav = Screen::Chores;
    uiDirty = true;
    return;
  }
  tft.fillScreen(BG);
  drawStatus(true);
  text(fit(p->name + " finished:", 304, SM), 160, 42, SM, MUTED, BG, MC_DATUM);
  if (textW(c->name, LG) <= 304) text(c->name, 160, 74, LG, TEXT, BG, MC_DATUM);
  else text2(c->name, 160, 74, 304, MD, TEXT, BG);
  text(fit(rewardText(*c, true), 304, SM), 160, 108, SM, GOOD, BG, MC_DATUM);
  button(R_YES, "Yes, it's done!", GO);
  button(R_NO, "Cancel", CARD2);
}

static void tapConfirm(int x, int y) {
  if (R_NO.has(x, y)) {
    click();
    nav = Screen::Chores;
    uiDirty = true;
  } else if (R_YES.has(x, y)) {
    Person* p = findPerson(curUser);
    ChoreDef* c = findChore(curChore);
    if (p && c && !choreDone(*p, *c)) {
      tone(SPEAKER, 2000, 60);
      queueChore(p->id, c->id);
      uiToast("Nice work, " + p->name + "!", c->name + ": " + rewardText(*c, true), true);
    }
    nav = Screen::Person;
    uiDirty = true;
  }
}

// ---- Still reading? ---------------------------------------------------------------------------

static void askDynamic() {
  Session* s = sess(askUser);
  if (!s) return;
  int32_t left = max<int32_t>(0, 180 - (int32_t)((millis() - s->askSinceMs) / 1000));
  text("Stops in " + clockText(left) + " if nobody answers", 160, 78, SM, MUTED, BG, MC_DATUM, 308);
}

static void drawAsk() {
  Person* p = findPerson(askUser);
  tft.fillScreen(BG);
  drawStatus(true);
  String q = "Still reading, " + (p ? p->name : String("")) + "?";
  Font qf = fitFont(q, 304, LG, MD);
  text(fit(q, 304, qf), 160, 48, qf, WARN, BG, MC_DATUM);
  button(R_ASK_YES, "Yes, still reading", GO);
  button(R_ASK_NO, "No, I stopped", CARD2);
  askDynamic();
}

static void tapAsk(int x, int y) {
  if (R_ASK_YES.has(x, y)) {
    click();
    confirmReading(askUser);
  } else if (R_ASK_NO.has(x, y)) {
    click();
    Session* s = findSession(askUser);
    Person* p = findPerson(askUser);
    if (s && p) {
      int32_t sec = sessionSeconds(*s);
      stopReading(askUser, false);
      uiToast("Reading saved", p->name + ": " + fmtDur(sec) + " read", true);
    }
  }
}

// ---- Toast ------------------------------------------------------------------------------------

static void drawToast() {
  tft.fillScreen(BG);
  drawStatus(true);
  Rect r{14, 34, 292, 166};
  card(r, CARD, BG, 16);
  uint16_t ic = toastGood ? GOOD : WARN;
  dot(160, 72, 24, ic, CARD);
  if (toastGood) checkIcon(160, 72, 11, onColor(ic), ic);
  else bangIcon(160, 72, 12, onColor(ic), ic);
  Font tf = fitFont(toastTitle, 270, LG, MD);
  text(fit(toastTitle, 270, tf), 160, 118, tf, TEXT, CARD, MC_DATUM);
  text2(toastDetail, 160, 160, 270, SM, MUTED, CARD);
  text("Tap anywhere to continue", 160, 220, SM, FAINT, BG, MC_DATUM);
}

// ---- Touch ------------------------------------------------------------------------------------

static void onTap(int x, int y) {
  Serial.printf("tap %d,%d\n", x, y);
  lastTouchMs = millis();
  if (previewing()) {  // any tap ends a preview
    previewUntil = 0;
    uiDirty = true;
    return;
  }
  switch (screen) {
    case Screen::Connect:
      if (bankState == BankState::Denied) bankForget();
      break;
    case Screen::Home: tapHome(x, y); break;
    case Screen::Person: tapPerson(x, y); break;
    case Screen::Reading: tapReading(x, y); break;
    case Screen::Chores: tapChores(x, y); break;
    case Screen::Confirm: tapConfirm(x, y); break;
    case Screen::Ask: tapAsk(x, y); break;
    case Screen::Toast:
      toastUntil = 0;
      uiDirty = true;
      break;
    default: break;
  }
}

void uiTap(int x, int y) { onTap(x, y); }

// Show a screen for 30 s with the first person and made-up reading data
bool uiPreview(const String& what) {
  static const struct {
    const char* name;
    Screen s;
  } screens[] = {{"connect", Screen::Connect}, {"home", Screen::Home},     {"person", Screen::Person},
                 {"reading", Screen::Reading}, {"chores", Screen::Chores}, {"confirm", Screen::Confirm},
                 {"ask", Screen::Ask},         {"toast", Screen::Toast}};
  previewUntil = 0;
  uiDirty = true;
  if (what == "off") return true;
  for (auto& e : screens) {
    if (what != e.name) continue;
    if (people.empty() && e.s != Screen::Connect) return false;
    previewScreen = e.s;
    curUser = askUser = people.empty() ? -1 : people[0].id;
    curChore = chores.empty() ? -1 : chores[0].id;
    previewSession = Session();
    previewSession.userId = curUser;
    previewSession.postedUntilMs = millis();
    previewSession.postedS = 754;
    if (e.s == Screen::Ask) {
      previewSession.asking = true;
      previewSession.askSinceMs = millis() - 40000;
      previewSession.postedUntilMs = previewSession.askSinceMs;
      previewSession.postedS = 1800;
    }
    if (e.s == Screen::Toast) {
      toastTitle = "Nice reading, " + people[0].name + "!";
      toastDetail = "25m read: " + earnedText(25 * 60);
      toastGood = true;
    }
    previewUntil = millis() + 30000;
    return true;
  }
  return false;
}

void uiCapture(Print& out) { capture(out); }

// Returns true and the screen position while the screen is touched
static bool readTouch(int& x, int& y) {
#ifdef WOKWI
  // Wokwi's display has an FT6206 capacitive touch chip on I2C (address 0x38), reporting
  // portrait panel coordinates; convert to our landscape screen (rotation 1).
  Wire.beginTransmission(0x38);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom(0x38, 5) != 5) return false;
  uint8_t b[5];
  for (auto& v : b) v = Wire.read();
  if ((b[0] & 0x0F) == 0) return false;
  int tx = ((b[1] & 0x0F) << 8) | b[2], ty = ((b[3] & 0x0F) << 8) | b[4];
  x = constrain(319 - ty, 0, 319);
  y = constrain(tx, 0, 239);
  return true;
#else
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 320), 0, 319);
  y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 240), 0, 239);
  return true;
#endif
}

// A tap counts once the finger has been down for 40 ms (filters brushes and glitches),
// at the position it has then; holding doesn't repeat.
static void pollTouch() {
  static uint32_t downAt = 0, lastSeen = 0;
  static bool fired = false;
  int x, y;
  if (readTouch(x, y)) {
    if (!downAt) {
      downAt = millis();
      fired = false;
    }
    lastSeen = millis();
    if (!fired && millis() - downAt >= 40) {
      fired = true;
      onTap(x, y);
    }
  } else if (downAt && millis() - lastSeen > 80) {
    downAt = 0;
  }
}

// ---- Main -------------------------------------------------------------------------------------

void uiBegin() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);

#ifdef WOKWI
  Wire.begin(27, 22);  // touch chip: SDA 27, SCL 22 (see diagram.json)
#else
  touchSpi.begin(XPT_CLK, XPT_MISO, XPT_MOSI, XPT_CS);
  ts.begin(touchSpi);
  ts.setRotation(1);
#endif

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(BG);
  text("Earning station", 160, 112, LG, TEXT, BG, MC_DATUM);
}

static Screen wantedScreen() {
  if (previewing()) return previewScreen;
  if (toastUntil && millis() < toastUntil) return Screen::Toast;
  toastUntil = 0;
  askUser = -1;
  for (auto& s : sessions)
    if (s.asking) {
      askUser = s.userId;
      return Screen::Ask;
    }
  if (people.empty()) return Screen::Connect;

  // Wander back home when left alone
  uint32_t idle = millis() - lastTouchMs;
  if ((nav == Screen::Person || nav == Screen::Chores || nav == Screen::Confirm) && idle > 40000) nav = Screen::Home;
  if (nav == Screen::Reading && idle > 120000) nav = Screen::Home;
  return nav;
}

void uiLoop() {
  static Screen lastDrawn = Screen::None;
  static int lastAsk = -1;
  if (previewUntil && !previewing()) {
    previewUntil = 0;
    uiDirty = true;
  }
  Screen want = wantedScreen();
  if (want != lastDrawn || uiDirty || (want == Screen::Ask && askUser != lastAsk)) {
    uiDirty = false;
    screen = want;
    lastAsk = askUser;
    switch (screen) {
      case Screen::Connect: drawConnect(); break;
      case Screen::Home: drawHome(); break;
      case Screen::Person: drawPerson(); break;
      case Screen::Reading: drawReading(); break;
      case Screen::Chores: drawChores(); break;
      case Screen::Confirm: drawConfirm(); break;
      case Screen::Ask: drawAsk(); break;
      case Screen::Toast: drawToast(); break;
      default: break;
    }
    lastDrawn = screen;
  }

  static uint32_t lastSec = 0;
  if (millis() - lastSec >= 1000) {
    lastSec = millis();
    if (screen == Screen::Reading) readingDynamic();
    else if (screen == Screen::Home) homeDynamic();
    else if (screen == Screen::Ask) askDynamic();
  }
  drawStatus(false);
  pollTouch();

  // Beep while asking "still reading?"; green blink while anyone reads
  static uint32_t nextBeep = 0;
  if (screen == Screen::Ask && !previewing() && millis() >= nextBeep) {
    tone(SPEAKER, 2400, 150);
    nextBeep = millis() + 15000;
  } else if (screen != Screen::Ask) {
    nextBeep = 0;
  }
  bool green = !sessions.empty() && (millis() / 1000) % 3 == 0;
  digitalWrite(LED_G, green ? LOW : HIGH);
  digitalWrite(LED_B, bankState == BankState::Pairing && (millis() / 500) % 2 ? LOW : HIGH);
}
