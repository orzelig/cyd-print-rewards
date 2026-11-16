#include "reading_core.h"

#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

namespace rr {

const char* statusName(Status s) {
  switch (s) {
    case OK: return "OK";
    case NETWORK: return "NETWORK";
    case SIGNED_OUT: return "SIGNED_OUT";
    case BOT_CHECK: return "BOT_CHECK";
    case SHAPE: return "SHAPE";
    case CONFIG: return "CONFIG";
  }
  return "?";
}

static time_t localMidnight(int year, int mon, int mday) {
  struct tm tm = {};
  tm.tm_year = year;
  tm.tm_mon = mon;
  tm.tm_mday = mday;  // mktime normalises overflow/underflow
  tm.tm_isdst = -1;
  return mktime(&tm);
}

Week weekContaining(time_t t, time_t now) {
  struct tm lt;
  localtime_r(&t, &lt);
  Week w;
  int sunday = lt.tm_mday - lt.tm_wday;
  for (int i = 0; i <= 7; i++) w.dayStart[i] = localMidnight(lt.tm_year, lt.tm_mon, sunday + i);
  w.start = w.dayStart[0];
  w.complete = now >= w.dayStart[7];
  w.queryEnd = w.complete ? w.dayStart[7] - 1 : now;
  w.daysCounted = 7;
  if (!w.complete) {
    for (int i = 0; i < 7; i++)
      if (now >= w.dayStart[i] && now < w.dayStart[i + 1]) w.daysCounted = i + 1;
  }
  struct tm st;
  localtime_r(&w.start, &st);
  char buf[16];
  strftime(buf, sizeof buf, "%Y-%m-%d", &st);
  w.startDate = buf;
  return w;
}

Week weekFor(time_t now, int weeksBack) {
  return weekContaining(now - (time_t)weeksBack * 7 * 86400, now);
}

std::string requestBody(const char* childId, const Week& w) {
  JsonDocument doc;
  doc["childDirectedId"] = childId;
  doc["startTime"] = (long long)w.start;
  doc["endTime"] = (long long)w.queryEnd;
  doc["aggregationInterval"] = 86400;
  doc["timeZone"] = TZ_NAME;
  std::string out;
  serializeJson(doc, out);
  return out;
}

static bool contains(const char* hay, const char* needle) { return hay && strstr(hay, needle) != nullptr; }

static bool containsNoCase(const char* hay, const char* needle) {
  if (!hay) return false;
  size_t n = strlen(needle);
  for (const char* p = hay; *p; p++) {
    size_t i = 0;
    while (i < n && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
    if (i == n) return true;
  }
  return false;
}

Status classifyHttp(int code, const char* location, const char* contentType, const char* body, std::string& err) {
  bool json = containsNoCase(contentType, "json");
  char buf[160];
  if (containsNoCase(location, "validatecaptcha") || contains(location, "/ap/cvf") ||
      (!json && containsNoCase(body, "captcha"))) {
    snprintf(buf, sizeof buf, "Amazon answered with a captcha / bot check (HTTP %d). Not attempting it.", code);
    err = buf;
    return BOT_CHECK;
  }
  if (code == 401 || code == 403 || contains(location, "/ap/signin") || (!json && contains(body, "ap_password")) ||
      (code >= 300 && code < 400)) {
    snprintf(buf, sizeof buf, "session cookie rejected (HTTP %d). Copy a fresh cookie with login_capture.py.", code);
    err = buf;
    return SIGNED_OUT;
  }
  if (code != 200) {
    snprintf(buf, sizeof buf, "unexpected HTTP %d", code);
    err = buf;
    return NETWORK;
  }
  if (!json) {
    err = "data shape changed: response is not JSON";
    return SHAPE;
  }
  return OK;
}

static Status shapeError(std::string& err, const char* what) {
  err = std::string("data shape changed: ") + what;
  return SHAPE;
}

Status parseActivity(const char* json, size_t len, const Week& w, KidReport& out, std::string& err) {
  // Keep only the fields we use: a few KB of JSON becomes well under 1 KB of document.
  JsonDocument filter;
  JsonObject cat = filter["activityV2Data"].add<JsonObject>();
  cat["category"] = true;
  JsonObject iv = cat["intervals"].add<JsonObject>();
  iv["startTime"] = true;
  iv["aggregatedDuration"] = true;
  JsonObject res = iv["aggregatedActivityResults"].add<JsonObject>();
  res["key"] = true;
  res["activityDuration"] = true;
  res["attributes"]["TITLE"] = true;

  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json, len, DeserializationOption::Filter(filter));
  if (e) return shapeError(err, (std::string("invalid JSON: ") + e.c_str()).c_str());
  JsonArray data = doc["activityV2Data"];
  if (data.isNull()) return shapeError(err, "missing activityV2Data");

  out.seconds = 0;
  for (double& d : out.daily) d = 0;
  out.titles.clear();
  for (JsonObject c : data) {
    const char* name = c["category"];
    if (!name) return shapeError(err, "missing category");
    JsonArray intervals = c["intervals"];
    if (intervals.isNull()) return shapeError(err, "missing intervals");
    if (strcmp(name, "BOOK") != 0) continue;
    for (JsonObject i : intervals) {
      if (!i["startTime"].is<double>()) return shapeError(err, "missing intervals[].startTime");
      JsonArray results = i["aggregatedActivityResults"];
      if (results.isNull()) return shapeError(err, "missing aggregatedActivityResults");
      time_t t = (time_t)i["startTime"].as<long long>();
      int day = -1;
      for (int d = 0; d < 7; d++)
        if (t >= w.dayStart[d] && t < w.dayStart[d + 1]) day = d;
      if (day < 0) continue;  // outside the requested week
      for (JsonObject r : results) {
        if (!r["activityDuration"].is<double>()) return shapeError(err, "missing activityDuration");
        if (r["attributes"].isNull()) return shapeError(err, "missing attributes");
        double secs = r["activityDuration"].as<double>();
        const char* title = r["attributes"]["TITLE"] | "(untitled)";
        const char* key = r["key"] | title;
        auto it = std::find_if(out.titles.begin(), out.titles.end(), [&](const Title& x) { return x.key == key; });
        if (it == out.titles.end()) {
          if (out.titles.size() < MAX_TITLES) {
            out.titles.push_back({key, title, 0});
            it = out.titles.end() - 1;
          } else {
            it = std::find_if(out.titles.begin(), out.titles.end(), [](const Title& x) { return x.key == "\x01other"; });
            if (it == out.titles.end()) {
              out.titles.back() = {"\x01other", "(other titles)", out.titles.back().seconds};
              it = out.titles.end() - 1;
            }
          }
        }
        it->seconds += secs;
        out.daily[day] += secs;
        out.seconds += secs;
      }
    }
  }
  std::sort(out.titles.begin(), out.titles.end(), [](const Title& a, const Title& b) { return a.seconds > b.seconds; });
  return OK;
}

int roundMinutes(double seconds) { return (int)floor(seconds / 60.0 + 0.5); }

int KidReport::totalMinutes() const { return roundMinutes(seconds); }

int KidReport::dailyAverageMinutes(const Week& w) const {
  return roundMinutes(w.daysCounted ? seconds / w.daysCounted : 0);
}

std::string formatMinutes(int minutes) {
  char buf[24];
  if (minutes >= 60)
    snprintf(buf, sizeof buf, "%dh %dm", minutes / 60, minutes % 60);
  else
    snprintf(buf, sizeof buf, "%dm", minutes);
  return buf;
}

std::string summaryLine(const KidReport& k, const Week& w) {
  return k.name + " " + formatMinutes(k.totalMinutes()) + " (daily avg " + formatMinutes(k.dailyAverageMinutes(w)) + ")";
}

std::string reportJson(const std::vector<KidReport>& kids, const Week& w) {
  JsonDocument doc;
  doc["timezone"] = TZ_NAME;
  doc["category"] = "Books";
  doc["week"]["start"] = w.startDate;
  doc["week"]["complete"] = w.complete;
  doc["week"]["days_counted"] = w.daysCounted;
  for (const KidReport& k : kids) {
    JsonObject o = doc["kids"][k.name].to<JsonObject>();
    o["total_minutes"] = k.totalMinutes();
    o["daily_average_minutes"] = k.dailyAverageMinutes(w);
    JsonArray daily = o["daily_minutes"].to<JsonArray>();
    for (int d = 0; d < w.daysCounted; d++) daily.add(roundMinutes(k.daily[d]));
    JsonArray titles = o["titles"].to<JsonArray>();
    for (const Title& t : k.titles) {
      JsonObject x = titles.add<JsonObject>();
      x["title"] = t.title;
      x["minutes"] = roundMinutes(t.seconds);
    }
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

}  // namespace rr
