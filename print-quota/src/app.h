// Shared types and functions for the print quota manager.
#pragma once
#define FW_VERSION __DATE__ " " __TIME__
#include <Arduino.h>
#include <vector>
#include <map>
#include <functional>
#include <ArduinoJson.h>

// ---- Users & quotas ----------------------------------------------------------

enum class Period : uint8_t { None, Day, Week, Month };

// Time left = quotaS - usedS + balanceS. Most users just have a balance (quotaS = 0,
// period None) that top-ups add to and printing spends; it may go negative.
// An optional allowance (quotaS > 0 with a period) resets usedS every period.
struct User {
  uint16_t id = 0;
  String name;
  int32_t quotaS = 0;        // optional allowance per period in seconds, -1 = no limit
  Period period = Period::None;
  int32_t usedS = 0;         // used in the current period (beyond quota + balance = negative)
  int32_t balanceS = 0;      // time added by top-ups
  uint32_t periodStart = 0;  // epoch of the current period's start, 0 = not anchored yet
  uint32_t totalS = 0;       // lifetime print time
  bool earns = true;         // listed on the earning station (parents can opt out)
  // Earning (reading / chores, from the earning station)
  int32_t filC = 0;          // filament money, cents
  int32_t cashC = 0;         // cash money, cents
  uint8_t filFrac = 0;       // filament remainder, 1/100 cent
  int32_t weekReadS = 0;     // reading this week (counts toward the cash goal)
  int32_t weekPaidC = 0;     // cash already paid for this week's reading
  int32_t prevReadS = 0;     // last week's reading, until last week closes on Sunday morning
  int32_t prevPaidC = 0;     // cash already paid for last week's reading
  uint32_t readTotalS = 0;   // lifetime reading
  uint32_t choreDay = 0;     // day the counts below belong to (yyyymmdd)
  std::map<uint16_t, uint8_t> choreCount;
};

constexpr size_t MAX_USERS = 40;

int32_t remainingS(const User& u);  // INT32_MAX when unlimited
bool canStart(const User& u);       // a print may only start with time left (> 0)
const char* periodName(Period p);
Period parsePeriod(const char* s);
uint32_t nextResetEpoch(const User& u);  // 0 if never / clock not set
void anchorPeriod(User& u);              // re-anchor after the period type changes

// ---- Settings -------------------------------------------------------------------

struct Settings {
  String printerIp, serial, accessCode;
  bool demo = false;
  uint16_t graceMin = 5;      // cancels within this many minutes cost nothing
  bool refundFailed = false;  // refund failed/cancelled prints entirely
  // Prints are never paused or stopped by this device: a print that started with
  // time left always finishes, even if the balance goes negative.
  String tz = "UTC0";         // POSIX TZ string, used for period resets
  String adminPass;           // empty = admin page open on the local network
};

// ---- Earning (earn.cpp) -------------------------------------------------------------
// Money is kept in cents; rates are in 1/100 cent per hour of reading.

struct Chore {
  uint16_t id = 0;
  String name;
  int32_t printS = 0;  // print time added
  int32_t filC = 0;    // filament money added, cents
  int32_t cashC = 0;   // cash added, cents
  uint8_t perDay = 1;  // how many times a day it counts (0 = unlimited)
};

struct EarnDevice {
  String name, key;
  String ip;  // last address it called from (not saved)
};

struct EarnRules {
  uint16_t printPct = 200;     // print seconds per reading second, in percent
  uint32_t filRate = 5000;     // filament: 1/100 cent per reading hour ($0.50)
  uint32_t cashRate = 10000;   // cash: 1/100 cent per reading hour ($1.00), paid only at the goal
  int32_t goalS = 7 * 3600;    // weekly reading goal for the cash
  uint16_t checkMin = 30;      // "still reading?" check interval, 0 = off
  uint32_t weekStart = 0;      // epoch of the current week's start (Sunday 00:00)
  uint32_t prevWeekStart = 0;  // last week, while it is still open for late Kindle reading (else 0)
  std::vector<Chore> chores;
  std::vector<EarnDevice> devices;
};

extern EarnRules earn;
void earnBegin();
void earnTick();
void saveEarn();
uint32_t weekEndEpoch();
int32_t weekPendingC(const User& u);  // cash waiting for the weekly goal
uint32_t prevWeekClosesEpoch();       // when last week's reading stops counting (0 = already closed)
void choreCountsToday(User& u);       // reset the per-day chore counts when the day changes
void earnRoutes();                    // registers the /api/earn/* routes
void earnUserJson(JsonObject o, User& u);
String earnPendingPair();             // code of a station waiting for approval, or ""

// ---- Printer --------------------------------------------------------------------

enum class PState : uint8_t { Offline, Idle, Prepare, Running, Pause, Finish, Failed };
const char* pstateName(PState s);

struct PrinterStatus {
  PState state = PState::Offline;
  int percent = 0;
  int remainingMin = -1;
  String job;
  int layer = 0, totalLayers = 0;
  String error;
};

// The print currently being charged
struct Job {
  bool active = false;
  int userId = -1;  // -1 = nobody picked yet
  String name;
  uint32_t startedEpoch = 0;
  int32_t chargedS = 0;
  bool alarmed = false;  // user hit zero during this job
  bool warned = false;   // 10-minute warning given
};

// ---- Globals (store.cpp) -----------------------------------------------------------

extern std::vector<User> users;
extern Settings settings;
extern PrinterStatus printer;
extern Job job;
extern int claimUserId;  // who tapped their name for the next print
extern uint32_t claimAtMs;
extern bool resultPending;  // show the "print finished" screen
extern bool resultOk;
extern String resultTitle, resultDetail;
extern int resultUserId;
extern uint32_t resultAtMs;
extern bool uiDirty;     // screen needs a full redraw
extern int chargeRate;   // seconds charged per real second (demo speed-up)
extern uint32_t historySeq;

// ---- store.cpp ------------------------------------------------------------------------

void storeBegin();
void saveUsers();
void saveSettings();
void usersChanged();  // after any edit: save soon and redraw
User* findUser(int id);
User* addUser(const String& name, int32_t quotaS, Period p);
bool deleteUser(int id);
void chargeUser(User& u, int32_t s);
void refundUser(User& u, int32_t s);
void assignJob(int userId);
void onPrinterState(PState prev, PState now);
void tickQuota();
void forEachHistoryLine(size_t newest, std::function<void(const String&)> fn);  // oldest first
void clearHistory();
void logTopup(const User& u, int32_t s, const String& note);
void logHistory(JsonDocument& e);  // append one entry to the history
bool timeValid();
uint32_t nowEpoch();
String fmtDur(int32_t s);       // "1h 05m" or "12m 05s"
String fmtDurShort(int32_t s);  // "1h 05m" or "12m"

// ---- printer.cpp ------------------------------------------------------------------------

void printerBegin();
void printerLoop();
extern uint32_t mqttMaxLen;  // largest printer message seen
void printerReconfigure();
void demoStart(int minutes, int speed);
void demoEnd(bool ok);
void demoTogglePause();

// ---- ui.cpp / web.cpp / main.cpp ----------------------------------------------------------

void uiBegin();
void uiLoop();
void uiTap(int x, int y);
bool uiPreview(const String& what);  // idle, unclaimed, printing, pick, result or off: show for 30 s
void uiCapture(Print& out);          // the screen as a BMP
constexpr size_t SCREEN_BMP_SIZE = 54 + 320 * 240 * 3;
void webLoop();
bool wifiPortalActive();
// Memory diagnostics: remembers which part of the program was running when free memory hit its low point
void heapMark(const char* where);
extern char heapLowAt[48];
String netStatus();
void applySettings();
