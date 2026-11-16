// Earning station: shared types.
#pragma once
#define FW_VERSION __DATE__ " " __TIME__
#include <Arduino.h>
#include <vector>
#include <map>
#include <ArduinoJson.h>

// Data mirrored from the print quota device ("bank")
struct Person {
  int id = 0;
  String name;
  int32_t leftS = 0;  // print time left
  bool unlimited = false;
  int32_t filC = 0, cashC = 0;  // cents
  int32_t weekReadS = 0, weekPendingC = 0, weekPaidC = 0;
  std::map<int, int> choresToday;
};

struct ChoreDef {
  int id = 0;
  String name;
  int32_t printS = 0, filC = 0, cashC = 0;
  int perDay = 1;
};

struct Rules {
  int printPct = 200;
  uint32_t filRate = 5000, cashRate = 10000;  // 1/100 cent per hour read
  int32_t goalS = 7 * 3600;
  int checkMin = 30;
  uint32_t weekEnd = 0, now = 0;  // bank's clock at the last fetch
  uint32_t fetchedMs = 0;
};

enum class BankState : uint8_t { NoWifi, Finding, Pairing, Denied, Ok, Offline };

// A reading session in progress
struct Session {
  int userId = -1;
  uint32_t startMs = 0;
  uint32_t postedUntilMs = 0;  // time up to here is already queued for the bank
  int32_t postedS = 0;         // seconds queued so far
  uint32_t nextCheckMs = 0;    // when to ask "still reading?"
  bool asking = false;
  uint32_t askSinceMs = 0;
};

extern std::vector<Person> people;
extern std::vector<ChoreDef> chores;
extern Rules rules;
extern BankState bankState;
extern String pairCode, bankError;
extern std::vector<Session> sessions;
extern bool uiDirty;

Person* findPerson(int id);
ChoreDef* findChore(int id);
String fmtDur(int32_t s);
String money(int32_t cents);
int32_t weekSecondsLeft();  // until the reading week ends, -1 if unknown

// bank.cpp
void bankBegin();
void bankLoop();
void bankForget();       // drop the pairing key
void bankRefreshSoon();
size_t queuedCount();
void queueRead(int userId, int32_t s, bool final, int32_t sessionS);
void queueChore(int userId, int choreId);
void queueKindle(int userId, int32_t s, int32_t extraAgeS);  // Kindle reading; extraAgeS dates it back
void bankStatus();       // print to serial
bool bankPaired();
bool bankKeyMatches(const String& k);

// reading.cpp
Session* findSession(int userId);
void startReading(int userId);
void stopReading(int userId, bool keepUnconfirmed);
void confirmReading(int userId);
void readingLoop();
int32_t sessionSeconds(const Session& s);  // counted time so far

// ui.cpp
void uiBegin();
void uiLoop();
void uiTap(int x, int y);
void uiToast(const String& title, const String& detail, bool good);
void uiMessage(const String& title, const String& detail);  // draw right away (e.g. while updating)
bool uiPreview(const String& what);  // show a screen for 30 s with made-up data: home, person, reading, chores, confirm, ask, toast, connect, off
void uiCapture(Print& out);          // the screen as a BMP
constexpr size_t SCREEN_BMP_SIZE = 54 + 320 * 240 * 3;

// amazon.cpp: Kindle reading from Amazon Kids, credited like reading here
enum class AmazonState : uint8_t { Off, Waiting, Ok, NoMatch, SignIn, BotCheck, Changed, Error };
void amazonBegin();
void amazonLoop();
AmazonState amazonState();
String amazonProblem();                  // short text for the screen, empty when fine
int32_t amazonWeekSeconds(int personId);  // Kindle time counted this week, -1 if none
bool amazonSetup(const String& json, String& err);  // store sign-in + kids (write-only), fetch right away
void amazonForget();
void amazonFetchNow();
void amazonStatus(JsonObject o);         // names, minutes, state, times; never the sign-in values

// web.cpp
void webLoop();

// main.cpp
bool wifiPortalActive();
