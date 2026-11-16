// Printer status: Bambu Lab local MQTT, or a simulated printer for testing.
//
// Bambu Lab printers run an MQTT broker on port 8883 (TLS, self-signed).
// Username "bblp", password = the LAN access code shown on the printer.
// Status arrives on device/<SERIAL>/report; P1/A1 printers send only changed
// fields, so we request a full "pushall" on connect and every few minutes.
#include "app.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WiFiClientSecure tls;
static PubSubClient mqtt(tls);
static uint32_t lastAttempt = 0, lastPush = 0;
uint32_t mqttMaxLen = 0;

// Demo printer
static int32_t demoTotal = 0, demoElapsed = 0;
static int demoSpeed = 1;
static uint32_t demoTick = 0, demoPrepareUntil = 0;

static void setState(PState s) {
  if (s == printer.state) return;
  PState prev = printer.state;
  printer.state = s;
  onPrinterState(prev, s);
}

static PState mapState(const char* s) {
  if (!strcmp(s, "RUNNING")) return PState::Running;
  if (!strcmp(s, "PREPARE") || !strcmp(s, "SLICING")) return PState::Prepare;
  if (!strcmp(s, "PAUSE")) return PState::Pause;
  if (!strcmp(s, "FINISH")) return PState::Finish;
  if (!strcmp(s, "FAILED")) return PState::Failed;
  return PState::Idle;
}

static String topic(const char* dir) { return "device/" + settings.serial + "/" + dir; }

static void pushall() {
  mqtt.publish(topic("request").c_str(),
               "{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}");
  lastPush = millis();
}

static void onMessage(char*, byte* payload, unsigned int len) {
  if (len > mqttMaxLen) mqttMaxLen = len;
  static JsonDocument filter;
  if (filter.isNull()) {
    JsonObject p = filter["print"].to<JsonObject>();
    p["gcode_state"] = true;
    p["mc_percent"] = true;
    p["mc_remaining_time"] = true;
    p["subtask_name"] = true;
    p["layer_num"] = true;
    p["total_layer_num"] = true;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload, len, DeserializationOption::Filter(filter))) return;
  JsonObject p = doc["print"];
  if (p.isNull()) return;
  if (p["mc_percent"].is<int>()) printer.percent = p["mc_percent"];
  if (p["mc_remaining_time"].is<int>()) printer.remainingMin = p["mc_remaining_time"];
  if (p["subtask_name"].is<const char*>()) printer.job = p["subtask_name"].as<String>();
  if (p["layer_num"].is<int>()) printer.layer = p["layer_num"];
  if (p["total_layer_num"].is<int>()) printer.totalLayers = p["total_layer_num"];
  if (p["gcode_state"].is<const char*>()) setState(mapState(p["gcode_state"]));
}

void printerBegin() {
  tls.setInsecure();  // the printer uses a self-signed certificate
  tls.setHandshakeTimeout(8);
  mqtt.setCallback(onMessage);
  mqtt.setBufferSize(16384);  // the P2S full status is ~7.8 KB (measured); bigger messages would be dropped
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(8);
}

void printerReconfigure() {
  if (mqtt.connected()) mqtt.disconnect();
  lastAttempt = 0;
  printer.error = "";
  chargeRate = 1;
  demoTotal = 0;
  setState(settings.demo ? PState::Idle : PState::Offline);
}

// ---- Demo printer --------------------------------------------------------------

void demoStart(int minutes, int speed) {
  minutes = constrain(minutes, 1, 24 * 60);
  demoTotal = minutes * 60;
  demoElapsed = 0;
  demoSpeed = constrain(speed, 1, 600);
  chargeRate = demoSpeed;
  printer.job = "Demo print (" + String(minutes) + " min)";
  printer.percent = 0;
  printer.remainingMin = minutes;
  demoPrepareUntil = millis() + 3000;
  setState(PState::Prepare);
}

void demoEnd(bool ok) {
  if (printer.state == PState::Running || printer.state == PState::Pause || printer.state == PState::Prepare)
    setState(ok ? PState::Finish : PState::Failed);
}

void demoTogglePause() {
  if (printer.state == PState::Running) setState(PState::Pause);
  else if (printer.state == PState::Pause) setState(PState::Running);
}

static void demoLoop() {
  if (printer.state == PState::Offline) setState(PState::Idle);
  if (printer.state == PState::Prepare && millis() > demoPrepareUntil) setState(PState::Running);
  if (millis() - demoTick < 1000) return;
  demoTick = millis();
  if (printer.state != PState::Running) return;
  demoElapsed += demoSpeed;
  if (demoElapsed >= demoTotal) {
    printer.percent = 100;
    printer.remainingMin = 0;
    setState(PState::Finish);
  } else {
    printer.percent = 100LL * demoElapsed / demoTotal;
    printer.remainingMin = (demoTotal - demoElapsed + 59) / 60;
  }
}

// ---- Main loop -------------------------------------------------------------------

void printerLoop() {
  if (settings.demo) {
    demoLoop();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setState(PState::Offline);
    return;
  }
  if (settings.printerIp.isEmpty() || settings.serial.isEmpty() || settings.accessCode.isEmpty()) {
    printer.error = "Printer not set up";
    setState(PState::Offline);
    return;
  }

  if (!mqtt.connected()) {
    setState(PState::Offline);  // keeps any active job; it resumes when we reconnect
    if (lastAttempt && millis() - lastAttempt < 20000) return;
    lastAttempt = millis();
    IPAddress ip;
    if (!ip.fromString(settings.printerIp)) {
      printer.error = "Bad printer IP address";
      return;
    }
    mqtt.setServer(ip, 8883);
    String clientId = "printquota-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    heapMark("before printer connect");
    bool ok = mqtt.connect(clientId.c_str(), "bblp", settings.accessCode.c_str());
    heapMark("printer connect");
    if (ok) {
      mqtt.subscribe(topic("report").c_str());
      printer.error = "";
      pushall();
      Serial.println("connected to printer");
    } else {
      int st = mqtt.state();
      if (st == 4 || st == 5) printer.error = "Printer rejected the access code";
      else if (st == -2) printer.error = "Can't reach printer at " + settings.printerIp;
      else printer.error = "Printer connection error " + String(st);
      Serial.println(printer.error);
    }
    return;
  }

  mqtt.loop();
  heapMark("printer message");
  if (millis() - lastPush > 5UL * 60 * 1000) pushall();
}
