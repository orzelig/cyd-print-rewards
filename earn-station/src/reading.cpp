// Reading sessions. Several people can read at once. Time is sent to the bank once a
// minute; every N minutes the station asks "still reading?" and, if nobody answers
// within 3 minutes, the session stops and the unanswered time doesn't count.
#include "station.h"

std::vector<Session> sessions;

static const uint32_t ASK_TIMEOUT_MS = 3UL * 60 * 1000;

Session* findSession(int userId) {
  for (auto& s : sessions)
    if (s.userId == userId) return &s;
  return nullptr;
}

static uint32_t checkIntervalMs() { return rules.checkMin > 0 ? rules.checkMin * 60000UL : 0; }

int32_t sessionSeconds(const Session& s) {
  uint32_t end = s.asking ? s.askSinceMs : millis();  // paused while asking
  return s.postedS + (int32_t)((end - s.postedUntilMs) / 1000);
}

void startReading(int userId) {
  if (findSession(userId) || !findPerson(userId)) return;
  Session s;
  s.userId = userId;
  s.startMs = s.postedUntilMs = millis();
  s.nextCheckMs = checkIntervalMs() ? millis() + checkIntervalMs() : 0;
  sessions.push_back(s);
  Serial.printf("reading started: user %d\n", userId);
  uiDirty = true;
}

void stopReading(int userId, bool keepUnconfirmed) {
  for (size_t i = 0; i < sessions.size(); i++) {
    Session& s = sessions[i];
    if (s.userId != userId) continue;
    uint32_t end = s.asking && !keepUnconfirmed ? s.askSinceMs : millis();
    int32_t chunk = end > s.postedUntilMs ? (end - s.postedUntilMs) / 1000 : 0;
    int32_t total = s.postedS + chunk;
    queueRead(userId, chunk, true, total);
    Serial.printf("reading stopped: user %d, %ld s\n", userId, (long)total);
    sessions.erase(sessions.begin() + i);
    uiDirty = true;
    return;
  }
}

void confirmReading(int userId) {
  Session* s = findSession(userId);
  if (!s) return;
  s->asking = false;
  s->nextCheckMs = checkIntervalMs() ? millis() + checkIntervalMs() : 0;
  uiDirty = true;
}

void readingLoop() {
  for (size_t i = 0; i < sessions.size(); i++) {
    Session& s = sessions[i];
    if (s.asking) {
      if (millis() - s.askSinceMs > ASK_TIMEOUT_MS) {
        int id = s.userId;
        Person* p = findPerson(id);
        String name = p ? p->name : String("Someone");
        stopReading(id, false);
        uiToast("Reading stopped", name + " didn't answer \"still reading?\"", false);
        i--;
      }
      continue;
    }
    if (millis() - s.postedUntilMs >= 60000) {
      int32_t chunk = (millis() - s.postedUntilMs) / 1000;
      queueRead(s.userId, chunk, false, 0);
      s.postedUntilMs += chunk * 1000;
      s.postedS += chunk;
    }
    if (s.nextCheckMs && (int32_t)(millis() - s.nextCheckMs) >= 0) {
      s.asking = true;
      s.askSinceMs = millis();
      uiDirty = true;
    }
  }
}
