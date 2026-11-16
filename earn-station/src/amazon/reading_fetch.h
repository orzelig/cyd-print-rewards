// Arduino-ESP32 glue: one HTTPS POST to Amazon Parent Dashboard per kid and week.
#pragma once
#include <Arduino.h>

#include "reading_core.h"

namespace rr {

struct Credentials {
  String atMain;    // value of the `at-main` cookie  (store in NVS, never in source)
  String ubidMain;  // value of the `ubid-main` cookie
};

// Blocking: TLS handshake ~1-2 s on an ESP32, plus ~0.2-0.3 s for the request.
// Needs Wi-Fi up and the clock set (TLS validity + week math): configTzTime(rr::TZ_POSIX, "pool.ntp.org").
// Never follows redirects; SIGNED_OUT / BOT_CHECK mean stop polling until a person fixes it.
Status fetchKidWeek(const char* childId, const Week& w, const Credentials& c, KidReport& out, std::string& err);

}  // namespace rr
