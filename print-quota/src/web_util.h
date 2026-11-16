// Shared bits of the web server for the route files
#pragma once
#include <WebServer.h>
#include <ArduinoJson.h>

extern WebServer server;
bool guard();  // admin password check
bool readBody(JsonDocument& d);
void sendJson(JsonDocument& d);
void sendOk();
void fail(int code, const char* msg);
