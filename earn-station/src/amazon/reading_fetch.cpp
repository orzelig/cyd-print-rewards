#include "reading_fetch.h"

#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#include "ca_certs.h"

namespace rr {

static const size_t MAX_RESPONSE = 48 * 1024;  // real responses are 2-5 KB per kid-week

Status fetchKidWeek(const char* childId, const Week& w, const Credentials& c, KidReport& out, std::string& err) {
  if (c.atMain.isEmpty() || c.ubidMain.isEmpty()) {
    err = "missing at-main / ubid-main cookie";
    return CONFIG;
  }
  WiFiClientSecure tls;
  tls.setCACert(AMAZON_ROOT_CAS);
  tls.setTimeout(20);  // seconds
  HTTPClient http;
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  http.setReuse(false);
  if (!http.begin(tls, ACTIVITY_URL)) {
    err = "http.begin failed";
    return NETWORK;
  }
  const char* keep[] = {"Location", "Content-Type"};
  http.collectHeaders(keep, 2);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "application/json");
  http.setUserAgent("reading-report-esp32/1.0");
  http.addHeader("Cookie", "at-main=" + c.atMain + "; ubid-main=" + c.ubidMain);

  std::string body = requestBody(childId, w);
  int code = http.POST((uint8_t*)body.data(), body.size());
  if (code < 0) {
    err = std::string("network: ") + HTTPClient::errorToString(code).c_str();
    http.end();
    return NETWORK;
  }
  int size = http.getSize();  // -1 when chunked
  if (size > (int)MAX_RESPONSE) {
    err = "response too large";
    http.end();
    return SHAPE;
  }
  String payload = http.getString();
  String location = http.header("Location");
  String contentType = http.header("Content-Type");
  http.end();

  Status s = classifyHttp(code, location.c_str(), contentType.c_str(), payload.c_str(), err);
  if (s != OK) return s;
  return parseActivity(payload.c_str(), payload.length(), w, out, err);
}

}  // namespace rr
