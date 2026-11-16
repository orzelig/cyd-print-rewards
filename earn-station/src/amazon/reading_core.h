// Portable core of the ESP32 reading report: no Arduino APIs, so it also builds on a
// desktop for tests. Mirrors reading_report.py / direct_report.py.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include <string>
#include <vector>

namespace rr {

// The household's time zone: weeks run Sunday 00:00 to Saturday 23:59 here. Set both in
// platformio.ini (build_flags): the POSIX rule for the ESP32 clock, and the IANA name Amazon expects.
#ifndef HOUSEHOLD_TZ_POSIX
#define HOUSEHOLD_TZ_POSIX "EST5EDT,M3.2.0,M11.1.0"
#endif
#ifndef HOUSEHOLD_TZ_NAME
#define HOUSEHOLD_TZ_NAME "America/New_York"
#endif
static const char* const TZ_POSIX = HOUSEHOLD_TZ_POSIX;
static const char* const TZ_NAME = HOUSEHOLD_TZ_NAME;
static const char* const HOST = "parents.amazon.com";
static const char* const ACTIVITY_URL = "https://parents.amazon.com/ajax/get-weekly-activities-v2";

// Same numbering as the Python scripts' exit codes.
enum Status {
  OK = 0,
  NETWORK = 1,      // Wi-Fi/DNS/TLS/timeout or unexpected HTTP status
  SIGNED_OUT = 3,   // 401/403 or redirect to sign-in: cookie expired or revoked
  BOT_CHECK = 4,    // captcha / bot check: stop, never attempt it
  SHAPE = 5,        // response format changed
  CONFIG = 6,       // missing Wi-Fi/cookie/kid settings
};
const char* statusName(Status s);

struct Week {
  time_t start = 0;      // Sunday 00:00 local
  time_t dayStart[8] = {};  // local midnights, [7] == end (exclusive)
  time_t queryEnd = 0;   // "now" for the running week, Saturday 23:59:59 for finished weeks
  bool complete = false;
  int daysCounted = 0;
  std::string startDate;  // YYYY-MM-DD
};
// Week (Sun-Sat, local TZ which must already be set) containing `t`; `now` decides "so far".
Week weekContaining(time_t t, time_t now);
// weeksBack = 0 for this week, 1 for last week.
Week weekFor(time_t now, int weeksBack);

std::string requestBody(const char* childId, const Week& w);

// Decide what an HTTP answer means before parsing it.
Status classifyHttp(int code, const char* location, const char* contentType, const char* body, std::string& err);

struct Title {
  std::string key;
  std::string title;
  double seconds = 0;
};
struct KidReport {
  std::string name;
  double seconds = 0;
  double daily[7] = {};
  std::vector<Title> titles;  // sorted by seconds, descending
  int totalMinutes() const;
  int dailyAverageMinutes(const Week& w) const;
};
static const size_t MAX_TITLES = 24;  // later ones are folded into "(other titles)"

// Parse a get-weekly-activities-v2 response; counts category BOOK only.
Status parseActivity(const char* json, size_t len, const Week& w, KidReport& out, std::string& err);

int roundMinutes(double seconds);
std::string formatMinutes(int minutes);  // "4h 22m" / "37m"
std::string summaryLine(const KidReport& k, const Week& w);  // "Sam 4h 22m (daily avg 37m)"
std::string reportJson(const std::vector<KidReport>& kids, const Week& w);

}  // namespace rr
