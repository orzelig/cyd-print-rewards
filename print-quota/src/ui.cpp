// Touchscreen UI: pick who's printing, live print/quota view, result screen.
// Every tap target is at least ~54 px tall (one finger), each person has their own color.
#include "app.h"
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>

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

enum class Screen { None, Idle, Printing, Pick, Result };
static Screen screen = Screen::None;
static bool picking = false;  // choosing who a running print belongs to
static int page = 0;
static int beepsLeft = 0;
static uint32_t nextBeep = 0;
static uint32_t noticeUntil = 0;  // a short message is showing in the title row

// Preview: show a screen for a while without changing anything (from the admin API)
static Screen previewScreen = Screen::None;
static uint32_t previewUntil = 0;
static int previewJobUser = -1;
static bool previewing() { return previewUntil && millis() < previewUntil; }
static int jobUser() { return previewing() ? previewJobUser : job.userId; }

// ---- Layout -------------------------------------------------------------------------

constexpr int TITLE_Y = 44;  // middle of the title row
const Rect R_CANCEL{206, 27, 108, 32};
const Rect R_BANNER{6, 30, 308, 52};
const Rect R_CHANGE{216, 37, 90, 38};
const Rect R_LEFT_CARD{6, 128, 151, 68}, R_TIME_CARD{163, 128, 151, 68};
const Rect R_BAND{6, 200, 308, 36};

// People are shown 6 to a screen (2 x 3); with more, the 6th tile becomes "More"
static int perPage() { return users.size() > 6 ? 5 : 6; }
static int pageCount() { return max(1, (int)((users.size() + perPage() - 1) / perPage())); }
static Rect slot(int i) { return {int16_t(6 + (i % 2) * 157), int16_t(62 + (i / 2) * 59), 151, 54}; }

// ---- Helpers -------------------------------------------------------------------------

static void click() { tone(SPEAKER, 3200, 25); }

static uint16_t quotaColor(const User& u) {
  if (u.quotaS < 0) return GOOD;
  int32_t r = remainingS(u);
  return r <= 0 ? BAD : r <= 600 ? WARN : GOOD;
}

static String leftText(const User& u) {
  if (u.quotaS < 0) return "No limit";
  int32_t r = remainingS(u);
  return r <= 0 ? String("No time left") : fmtDurShort(r) + " left";
}

// ---- Status bar and title row -------------------------------------------------------------

static String shownL, shownR;

static String statusLeft() {
  String s;
  if (printer.state == PState::Offline) {
    s = printer.error.length() ? printer.error : String("Printer offline");
  } else {
    s = pstateName(printer.state);
    if (printer.state == PState::Running || printer.state == PState::Pause) s += " " + String(printer.percent) + "%";
  }
  return settings.demo ? "Demo: " + s : s;
}

static uint16_t stateColor() {
  switch (printer.state) {
    case PState::Running: return GOOD;
    case PState::Finish: return GOOD;
    case PState::Pause: return WARN;
    case PState::Prepare: return SKY;
    case PState::Failed: return BAD;
    case PState::Offline: return BAD;
    default: return FAINT;
  }
}

static void drawStatus(bool force) {
  String l = statusLeft(), r = netStatus();
  if (!force && l == shownL && r == shownR) return;
  shownL = l;
  shownR = r;
  tft.fillRect(0, 0, 320, 26, BG);
  dot(12, 13, 5, stateColor(), BG);
  text(r, 314, 13, SM, FAINT, BG, MR_DATUM);
  text(fit(l, 320 - textW(r, SM) - 44, SM), 24, 13, SM, MUTED, BG, ML_DATUM);
}

static void title(const String& s, uint16_t c, int maxW = 304) {
  tft.fillRect(0, 27, 320, 33, BG);
  text(fit(s, maxW, MD), 8, TITLE_Y, MD, c, BG, ML_DATUM);
}

// Short message in the title row; the screen is redrawn after 3 seconds
static void notice(const String& msg, uint16_t c) {
  tft.fillRect(0, 27, 320, 33, BG);
  text(fit(msg, 304, SM), 8, TITLE_Y, SM, c, BG, ML_DATUM);
  noticeUntil = millis() + 3000;
}

static void refuseNoTime(const User& u) {
  tone(SPEAKER, 400, 250);
  notice(u.name + " has no time left - ask for a top-up", SOFT_RED);
}

static void noUsersMessage() {
  text("No people yet", 160, 115, LG, TEXT, BG, MC_DATUM);
  text("Add them at http://printquota.local", 160, 150, SM, MUTED, BG, MC_DATUM);
}

// ---- People grid ---------------------------------------------------------------------------

static void personTile(const Rect& r, const User& u, bool selected, const char* selectedText) {
  bool ok = canStart(u) || selected;
  uint16_t c = personColor(u.id), bg = ok ? c : CARD;
  card(r, bg);
  if (selected) {
    tft.drawRoundRect(r.x + 3, r.y + 3, r.w - 6, r.h - 6, 9, onColor(bg));
    tft.drawRoundRect(r.x + 4, r.y + 4, r.w - 8, r.h - 8, 8, onColor(bg));
  }
  avatar(r.x + 24, r.cy(), 15, ok ? c : CARD2, u.name, ok, bg);
  int tx = r.x + 46, tw = r.w - 52;
  text(fit(u.name, tw, MD), tx, r.y + 18, MD, ok ? onColor(bg) : MUTED, bg, ML_DATUM);
  String line = selected ? String(selectedText) : ok ? leftText(u) : String("Needs top-up");
  text(fit(line, tw, SM), tx, r.y + 39, SM, ok ? softOn(bg) : SOFT_RED, bg, ML_DATUM);
}

static void drawGrid(int selectedId, const char* selectedText = "") {
  page = constrain(page, 0, pageCount() - 1);
  int n = perPage();
  for (int i = 0; i < n; i++) {
    size_t idx = page * n + i;
    if (idx >= users.size()) break;
    personTile(slot(i), users[idx], users[idx].id == selectedId, selectedText);
  }
  if (pageCount() > 1) {
    Rect r = slot(5);
    card(r, CARD2);
    text("More", r.x + 16, r.cy() - 9, MD, TEXT, CARD2, ML_DATUM);
    text(String(page + 1) + " of " + String(pageCount()), r.x + 16, r.cy() + 13, SM, MUTED, CARD2, ML_DATUM);
    chevron(r.x + r.w - 24, r.cy(), 10, false, TEXT, CARD2);
  }
}

// The id of the person tapped, or -1. Tapping "More" turns the page.
static int gridTap(int x, int y) {
  if (pageCount() > 1 && slot(5).has(x, y)) {
    click();
    page = (page + 1) % pageCount();
    uiDirty = true;
    return -1;
  }
  int n = perPage();
  for (int i = 0; i < n; i++) {
    size_t idx = page * n + i;
    if (idx < users.size() && slot(i).has(x, y)) return users[idx].id;
  }
  return -1;
}

// ---- Idle: tap your name before printing -------------------------------------------------------

static void drawIdle() {
  tft.fillScreen(BG);
  drawStatus(true);
  if (users.empty()) return noUsersMessage();
  User* claimed = findUser(claimUserId);
  if (claimed) title(claimed->name + ", start your print now!", personColor(claimed->id));
  else title("Who's printing next?", TEXT);
  drawGrid(claimUserId, "Up next!");
}

static void tapIdle(int x, int y) {
  int id = gridTap(x, y);
  User* u = findUser(id);
  if (!u) return;
  if (claimUserId != id && !canStart(*u)) return refuseNoTime(*u);
  click();
  claimUserId = claimUserId == id ? -1 : id;
  claimAtMs = millis();
  uiDirty = true;
}

// Assign the running print to whoever was tapped
static void pickAt(int x, int y) {
  int id = gridTap(x, y);
  User* u = findUser(id);
  if (!u) return;
  if (id != job.userId && !canStart(*u)) return refuseNoTime(*u);
  click();
  assignJob(id);
  picking = false;
  uiDirty = true;
}

// ---- Printing: live view --------------------------------------------------------------------

static String shownBand;
static int shownPct = -1;

static void printingDynamic(bool force) {
  User* u = findUser(jobUser());
  if (!u) return;

  String name = job.name.length() ? job.name : printer.job;
  if (name.isEmpty()) name = "Print job";
  int pct = constrain(printer.percent, 0, 100);
  text(fit(name, 228, SM), 8, 98, SM, MUTED, BG, ML_DATUM, 230);
  text(String(pct) + "%", 312, 98, MD, TEXT, BG, MR_DATUM, 74);
  if (force || pct != shownPct) {
    shownPct = pct;
    bar(8, 112, 304, 10, pct * 10, printer.state == PState::Pause ? WARN : GOOD, CARD2, BG);
  }

  int32_t printLeftS = printer.remainingMin >= 0 ? printer.remainingMin * 60 : -1;
  text(printLeftS >= 0 ? fmtDurShort(printLeftS) : String("--"), R_LEFT_CARD.cx(), R_LEFT_CARD.y + 47, LG, TEXT, CARD,
       MC_DATUM, R_LEFT_CARD.w - 14);
  String tl = u->quotaS < 0 ? String("No limit") : fmtDur(remainingS(*u));
  text(tl, R_TIME_CARD.cx(), R_TIME_CARD.y + 47, LG, quotaColor(*u), CARD, MC_DATUM, R_TIME_CARD.w - 14);

  // The print is never stopped; this is information only
  String msg;
  uint16_t bc = 0;
  if (u->quotaS >= 0 && remainingS(*u) <= 0) {
    msg = "Out of time - this print goes negative";
    bc = BAD;
  } else if (u->quotaS >= 0 && printLeftS > remainingS(*u)) {
    msg = "Will end " + fmtDurShort(printLeftS - remainingS(*u)) + " in the negative";
    bc = WARN;
  } else {
    msg = "Charged so far: " + fmtDurShort(job.chargedS);
  }
  String key = msg + String(bc);
  if (!force && key == shownBand) return;
  shownBand = key;
  tft.fillRect(0, R_BAND.y, 320, R_BAND.h, BG);
  if (bc) {
    card(R_BAND, bc);
    text(fit(msg, R_BAND.w - 20, SM), R_BAND.cx(), R_BAND.cy(), SM, onColor(bc), bc, MC_DATUM);
  } else {
    text(msg, 160, R_BAND.cy(), SM, FAINT, BG, MC_DATUM);
  }
}

static void drawPrinting() {
  tft.fillScreen(BG);
  drawStatus(true);
  User* u = findUser(jobUser());
  if (!u) {  // nobody claimed it: pick right here
    title("Who's printing this?", WARN);
    if (users.empty()) return noUsersMessage();
    return drawGrid(-1);
  }
  uint16_t c = personColor(u->id);
  card(R_BANNER, c);
  avatar(R_BANNER.x + 28, R_BANNER.cy(), 18, c, u->name, true, c);
  text(fit(u->name, R_CHANGE.x - 66, LG), 58, R_BANNER.cy() + 1, LG, onColor(c), c, ML_DATUM);
  button(R_CHANGE, "Change", mix(INK, c, 90), MD, c);

  card(R_LEFT_CARD, CARD);
  text("Print left", R_LEFT_CARD.cx(), R_LEFT_CARD.y + 14, SM, MUTED, CARD, MC_DATUM);
  card(R_TIME_CARD, CARD);
  text(fit(u->name + "'s time", R_TIME_CARD.w - 14, SM), R_TIME_CARD.cx(), R_TIME_CARD.y + 14, SM, MUTED, CARD, MC_DATUM);
  shownBand = "";
  shownPct = -1;
  printingDynamic(true);
}

static void tapPrinting(int x, int y) {
  if (!findUser(jobUser())) return pickAt(x, y);
  if (R_BANNER.has(x, y) && !users.empty()) {
    click();
    picking = true;
    page = 0;
    uiDirty = true;
  }
}

// ---- Change who a running print belongs to ----------------------------------------------------

static void drawPick() {
  tft.fillScreen(BG);
  drawStatus(true);
  title("Who's printing?", TEXT, R_CANCEL.x - 14);
  button(R_CANCEL, "Cancel", CARD2);
  drawGrid(job.userId, "Printing");
}

static void tapPick(int x, int y) {
  if (R_CANCEL.has(x, y)) {
    click();
    picking = false;
    uiDirty = true;
    return;
  }
  pickAt(x, y);
}

// ---- Result ------------------------------------------------------------------------------------

static void drawResult(const String& t, const String& detail, int userId, bool ok) {
  tft.fillScreen(BG);
  drawStatus(true);
  Rect r{14, 34, 292, 166};
  card(r, CARD, BG, 16);
  uint16_t ic = ok ? GOOD : WARN;
  dot(160, 72, 24, ic, CARD);
  if (ok) checkIcon(160, 72, 11, onColor(ic), ic);
  else bangIcon(160, 72, 12, onColor(ic), ic);
  Font tf = fitFont(t, 270, LG, MD);
  text(fit(t, 270, tf), 160, 118, tf, TEXT, CARD, MC_DATUM);
  text(fit(detail, 270, SM), 160, 148, SM, MUTED, CARD, MC_DATUM);
  if (User* u = findUser(userId))
    text(fit(u->name + ": " + leftText(*u), 270, MD), 160, 177, MD, quotaColor(*u), CARD, MC_DATUM);
  text("Tap anywhere to continue", 160, 220, SM, FAINT, BG, MC_DATUM);
}

// ---- Touch ------------------------------------------------------------------------------------

static bool readTouch(int& x, int& y) {
  if (!ts.touched()) return false;
  TS_Point p = ts.getPoint();
  x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 320), 0, 319);
  y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 240), 0, 239);
  return true;
}

static void onTap(int x, int y) {
  Serial.printf("tap %d,%d\n", x, y);
  if (previewing()) {  // any tap ends a preview
    previewUntil = 0;
    uiDirty = true;
    return;
  }
  switch (screen) {
    case Screen::Idle: tapIdle(x, y); break;
    case Screen::Printing: tapPrinting(x, y); break;
    case Screen::Pick: tapPick(x, y); break;
    case Screen::Result:
      resultPending = false;
      uiDirty = true;
      break;
    default: break;
  }
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

// ---- Alerts ------------------------------------------------------------------------------------

static void updateAlerts() {
  static bool lastAlarmed = false, lastWarned = false;
  if (job.alarmed && !lastAlarmed) beepsLeft = 6;
  else if (job.warned && !lastWarned) beepsLeft = 2;
  lastAlarmed = job.alarmed;
  lastWarned = job.warned;

  if (beepsLeft > 0 && millis() >= nextBeep) {
    tone(SPEAKER, 2600, 180);
    beepsLeft--;
    nextBeep = millis() + 350;
  }

  bool redOn = job.active && job.alarmed && (millis() / 400) % 2 == 0;
  bool greenOn = job.active && !job.alarmed && printer.state == PState::Running && (millis() / 1000) % 4 == 0;
  digitalWrite(LED_R, redOn ? LOW : HIGH);
  digitalWrite(LED_G, greenOn ? LOW : HIGH);
}

// ---- Remote control (admin API) -----------------------------------------------------------------

void uiTap(int x, int y) { onTap(x, y); }

bool uiPreview(const String& what) {
  previewJobUser = job.userId;
  if (what == "idle") previewScreen = Screen::Idle;
  else if (what == "unclaimed") previewScreen = Screen::Printing, previewJobUser = -1;
  else if (what == "printing") {
    previewScreen = Screen::Printing;
    if (!findUser(previewJobUser) && !users.empty()) previewJobUser = users[0].id;
  } else if (what == "pick") previewScreen = Screen::Pick;
  else if (what == "result") previewScreen = Screen::Result;
  else {
    previewUntil = 0;
    uiDirty = true;
    return what == "off";
  }
  previewUntil = millis() + 30000;
  uiDirty = true;
  return true;
}

void uiCapture(Print& out) { capture(out); }

// ---- Main ---------------------------------------------------------------------------------------

void uiBegin() {
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, HIGH);

  touchSpi.begin(XPT_CLK, XPT_MISO, XPT_MOSI, XPT_CS);
  ts.begin(touchSpi);
  ts.setRotation(1);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(BG);
  text("Print Quota", 160, 112, LG, TEXT, BG, MC_DATUM);
}

void uiLoop() {
  if (resultPending && millis() - resultAtMs > 20000) resultPending = false;
  if (!job.active) picking = false;
  if (previewUntil && !previewing()) {
    previewUntil = 0;
    uiDirty = true;
  }
  if (noticeUntil && millis() > noticeUntil) {
    noticeUntil = 0;
    uiDirty = true;
  }

  Screen want = previewing()    ? previewScreen
                : resultPending ? Screen::Result
                : job.active    ? (picking ? Screen::Pick : Screen::Printing)
                                : Screen::Idle;
  static int drawnJobUser = -2;
  if (want == Screen::Printing && jobUser() != drawnJobUser) uiDirty = true;  // e.g. assigned on the admin page
  if (want != screen || uiDirty) {
    screen = want;
    uiDirty = false;
    noticeUntil = 0;
    drawnJobUser = jobUser();
    switch (screen) {
      case Screen::Idle: drawIdle(); break;
      case Screen::Printing: drawPrinting(); break;
      case Screen::Pick: drawPick(); break;
      case Screen::Result:
        if (previewing())
          drawResult("Print finished", "Charged 1h 20m for Plant pot", users.empty() ? -1 : users[0].id, true);
        else
          drawResult(resultTitle, resultDetail, resultUserId, resultOk);
        break;
      default: break;
    }
  }

  static uint32_t lastDyn = 0;
  if (screen == Screen::Printing && millis() - lastDyn >= 500) {
    lastDyn = millis();
    printingDynamic(false);
  }
  drawStatus(false);
  pollTouch();
  updateAlerts();
}
