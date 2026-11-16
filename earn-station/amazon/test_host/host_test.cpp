// Desktop test for reading_core: runs every case in a spec file produced by
// make_spec.py, where the expected values come from the Python implementation.
//   c++ -std=c++17 -I../../src/amazon -I<ArduinoJson/src> host_test.cpp ../../src/amazon/reading_core.cpp -o host_test
//   ./host_test spec.json
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fstream>
#include <sstream>
#include <string>

#include <ArduinoJson.h>

#include "reading_core.h"

static int failures = 0, passes = 0;
#define CHECK(cond, ...)                         \
  do {                                           \
    if (cond) {                                  \
      passes++;                                  \
    } else {                                     \
      failures++;                                \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                       \
      printf("\n");                              \
    }                                            \
  } while (0)

static std::string slurp(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: host_test spec.json\n");
    return 2;
  }
  setenv("TZ", rr::TZ_POSIX, 1);
  tzset();
  JsonDocument spec;
  std::string specText = slurp(argv[1]);
  if (deserializeJson(spec, specText)) {
    fprintf(stderr, "bad spec\n");
    return 2;
  }

  for (JsonObject c : spec["weeks"].as<JsonArray>()) {
    rr::Week w = rr::weekContaining((time_t)c["t"].as<long long>(), (time_t)c["now"].as<long long>());
    const char* label = c["label"];
    CHECK(w.start == c["start"].as<long long>(), "%s start %lld != %lld", label, (long long)w.start, c["start"].as<long long>());
    CHECK(w.queryEnd == c["query_end"].as<long long>(), "%s queryEnd %lld != %lld", label, (long long)w.queryEnd, c["query_end"].as<long long>());
    CHECK(w.daysCounted == c["days_counted"].as<int>(), "%s days %d != %d", label, w.daysCounted, c["days_counted"].as<int>());
    CHECK(w.complete == c["complete"].as<bool>(), "%s complete", label);
    CHECK(w.startDate == c["start_date"].as<const char*>(), "%s startDate %s", label, w.startDate.c_str());
    CHECK(w.dayStart[7] - w.dayStart[0] == c["span"].as<long long>(), "%s span", label);
  }

  for (JsonObject c : spec["parses"].as<JsonArray>()) {
    const char* label = c["label"];
    rr::Week w = rr::weekContaining((time_t)c["t"].as<long long>(), (time_t)c["now"].as<long long>());
    std::string body = slurp(c["file"]);
    rr::KidReport k;
    k.name = c["name"].as<const char*>();
    std::string err;
    rr::Status s = rr::parseActivity(body.data(), body.size(), w, k, err);
    CHECK(s == c["status"].as<int>(), "%s status %d (%s) != %d", label, s, err.c_str(), c["status"].as<int>());
    if (s != rr::OK) continue;
    CHECK(k.totalMinutes() == c["total_minutes"].as<int>(), "%s total %d != %d", label, k.totalMinutes(), c["total_minutes"].as<int>());
    CHECK((long long)k.seconds == c["total_seconds"].as<long long>(), "%s seconds", label);
    CHECK(k.dailyAverageMinutes(w) == c["avg_minutes"].as<int>(), "%s avg %d != %d", label, k.dailyAverageMinutes(w), c["avg_minutes"].as<int>());
    CHECK(rr::summaryLine(k, w) == c["summary"].as<const char*>(), "%s summary '%s' != '%s'", label, rr::summaryLine(k, w).c_str(), c["summary"].as<const char*>());
    JsonArray titles = c["titles"];
    CHECK(k.titles.size() == titles.size(), "%s title count %zu != %zu", label, k.titles.size(), titles.size());
    for (size_t i = 0; i < titles.size() && i < k.titles.size(); i++)
      CHECK(rr::roundMinutes(k.titles[i].seconds) == titles[i]["minutes"].as<int>(), "%s title %zu minutes", label, i);
    JsonArray daily = c["daily"];
    for (size_t d = 0; d < daily.size(); d++)
      CHECK(rr::roundMinutes(k.daily[d]) == daily[d].as<int>(), "%s day %zu: %d != %d", label, d, rr::roundMinutes(k.daily[d]), daily[d].as<int>());
  }

  for (JsonObject c : spec["classify"].as<JsonArray>()) {
    std::string err;
    rr::Status s = rr::classifyHttp(c["code"], c["location"] | "", c["content_type"] | "", c["body"] | "", err);
    CHECK(s == c["status"].as<int>(), "classify %s -> %d (%s), want %d", c["label"].as<const char*>(), s, err.c_str(), c["status"].as<int>());
  }

  // Request body round-trip
  {
    rr::Week w = rr::weekContaining(1794849336, 1794849336);
    std::string b = rr::requestBody("amzn1.account.TEST", w);
    JsonDocument d;
    deserializeJson(d, b);
    struct tm sun = {};  // Sunday 2026-11-15 00:00 in the household time zone
    sun.tm_year = 2026 - 1900; sun.tm_mon = 10; sun.tm_mday = 15; sun.tm_isdst = -1;
    CHECK(d["startTime"].as<long long>() == (long long)mktime(&sun), "body startTime %s", b.c_str());
    CHECK(d["endTime"].as<long long>() == 1794849336LL, "body endTime");
    CHECK(d["aggregationInterval"].as<int>() == 86400 && strcmp(d["timeZone"], rr::TZ_NAME) == 0, "body fields");
  }

  printf("%d checks passed, %d failed\n", passes, failures);
  return failures ? 1 : 0;
}
