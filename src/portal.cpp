#include "portal.h"

#include <DNSServer.h>
#include <WebServer.h>
#include <WiFi.h>

#include <algorithm>
#include <vector>

#include "portal_html.h"
#include "protocol.h"

#ifndef MURMUR_FW_VERSION
#define MURMUR_FW_VERSION "dev"
#endif

namespace portal {
namespace {

WebServer *server = nullptr;
DNSServer *dns = nullptr;
Settings *cfg = nullptr;
String id;
String error;
std::vector<String> ssids;  // last scan, strongest first
bool saved = false;
uint32_t lastActivity = 0;

String htmlEscape(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (char c : in) {
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += c;
    }
  }
  return out;
}

void scanNetworks() {
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n >= 0) {
    ssids.clear();
    for (int i = 0; i < n; i++) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() && std::find(ssids.begin(), ssids.end(), ssid) == ssids.end()) ssids.push_back(ssid);
    }
    WiFi.scanDelete();
  }
  WiFi.scanNetworks(true);  // refresh in the background
}

void sendPage(const String &body, int code = 200) {
  String page;
  page.reserve(strlen_P(kPortalHead) + body.length() + 32);
  page += FPSTR(kPortalHead);
  page += body;
  page += FPSTR(kPortalTail);
  server->sendHeader("Cache-Control", "no-store");
  server->send(code, "text/html; charset=utf-8", page);
}

void handleRoot() {
  lastActivity = millis();
  String body = FPSTR(kPortalForm);
  String sensors;
  for (uint8_t i = 0; i < kSensorTypeCount; i++) {
    const auto t = static_cast<SensorType>(i);
    sensors += "<option value=\"" + String(sensorKey(t)) + "\"" + (t == cfg->sensor ? " selected" : "") + ">" +
               sensorLabel(t) + "</option>";
  }
  body.replace("{{FW}}", MURMUR_FW_VERSION);
  body.replace("{{ID}}", id);
  body.replace("{{ERROR}}", error.length() ? "<div class=\"err\">" + htmlEscape(error) + "</div>" : "");
  body.replace("{{SENSORS}}", sensors);
  body.replace("{{NAME}}", htmlEscape(cfg->name));
  body.replace("{{INTERVAL}}", String(cfg->interval));
  body.replace("{{BATTERY}}", cfg->battery ? "checked" : "");
  body.replace("{{SSID}}", htmlEscape(cfg->ssid));
  String options;
  for (const String &ssid : ssids) options += "<option value=\"" + htmlEscape(ssid) + "\">";
  body.replace("{{NETWORKS}}", options);
  body.replace("{{PASS_PH}}", cfg->pass.length() ? "Unchanged" : "Wi-Fi password");
  body.replace("{{HOST}}", htmlEscape(cfg->mqttHost));
  body.replace("{{PORT}}", String(cfg->mqttPort));
  body.replace("{{USER}}", htmlEscape(cfg->mqttUser));
  body.replace("{{MPASS_PH}}", cfg->mqttPass.length() ? "Unchanged" : "optional");
  body.replace("{{BASE}}", htmlEscape(cfg->baseTopic));
  body.replace("{{PDATA}}", String(cfg->pinData));
  body.replace("{{PSDA}}", String(cfg->pinSda));
  body.replace("{{PSCL}}", String(cfg->pinScl));
  body.replace("{{OTA_PH}}", cfg->otaPass.length() ? "Unchanged" : "optional");
  body.replace("{{LOWTX}}", cfg->lowTx ? "checked" : "");
  sendPage(body);
}

void handleScan() {
  lastActivity = millis();
  String json = "[";
  for (size_t i = 0; i < ssids.size(); i++) {
    String ssid = ssids[i];
    ssid.replace("\\", "\\\\");
    ssid.replace("\"", "\\\"");
    json += (i ? ",\"" : "\"") + ssid + "\"";
  }
  json += "]";
  server->send(200, "application/json", json);
}

bool parsePin(const char *field, uint8_t &out) {
  const int v = server->arg(field).toInt();
  if (!server->hasArg(field) || server->arg(field).isEmpty()) return true;
  if (!pinAllowed(v)) {
    error = String("GPIO") + v + " can't be used. Pick 0-7, 10, 20 or 21 (2, 8 and 9 are reserved).";
    return false;
  }
  out = v;
  return true;
}

void handleSave() {
  lastActivity = millis();
  error = "";
  Settings next = *cfg;

  next.sensor = sensorFromKey(server->arg("sensor"));
  next.name = server->arg("name");
  next.name.trim();
  next.name = next.name.substring(0, murmur::namePrefixBytes(next.name.c_str()));
  if (next.name.isEmpty()) next.name = id;
  next.interval = constrain(server->arg("interval").toInt(), (long)kIntervalMin, (long)kIntervalMax);
  next.battery = server->hasArg("battery");
  next.ssid = server->arg("ssid");
  next.ssid.trim();
  if (server->arg("pass").length()) next.pass = server->arg("pass");
  next.mqttHost = server->arg("host");
  next.mqttHost.trim();
  const long port = server->arg("port").toInt();
  next.mqttPort = (port > 0 && port < 65536) ? port : 1883;
  next.mqttUser = server->arg("user");
  next.mqttUser.trim();
  if (server->arg("mpass").length()) next.mqttPass = server->arg("mpass");
  if (next.mqttUser.isEmpty()) next.mqttPass = "";
  next.baseTopic = server->arg("base");
  next.baseTopic.trim();
  while (next.baseTopic.startsWith("/")) next.baseTopic.remove(0, 1);
  while (next.baseTopic.endsWith("/")) next.baseTopic.remove(next.baseTopic.length() - 1);
  if (next.baseTopic.isEmpty()) next.baseTopic = "murmur";
  if (server->arg("ota").length()) next.otaPass = server->arg("ota");
  next.lowTx = server->hasArg("lowtx");

  bool ok = parsePin("pdata", next.pinData) && parsePin("psda", next.pinSda) && parsePin("pscl", next.pinScl);
  if (ok && next.pinSda == next.pinScl) { error = "SDA and SCL must be different pins."; ok = false; }
  if (ok && next.ssid.isEmpty()) { error = "Enter your Wi-Fi network name."; ok = false; }
  if (ok && next.mqttHost.isEmpty()) { error = "Enter the MQTT broker address."; ok = false; }
  if (ok && (next.baseTopic.indexOf('+') >= 0 || next.baseTopic.indexOf('#') >= 0)) {
    error = "The topic root can't contain + or #.";
    ok = false;
  }
  if (!ok) {
    *cfg = next;  // keep what was typed
    handleRoot();
    return;
  }

  next.configured = true;
  *cfg = next;
  cfg->save();
  String body = FPSTR(kPortalSaved);
  body.replace("{{SSID}}", htmlEscape(cfg->ssid));
  String suffix = id.substring(id.length() - 6);
  suffix.toUpperCase();
  body.replace("{{SHORT}}", suffix);
  sendPage(body);
  saved = true;
}

void redirectHome() {
  server->sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  server->send(302, "text/plain", "");
}

}  // namespace

Result run(Settings &settings, const String &nodeId, StatusLed &led, uint32_t idleTimeoutMs, bool keepTryingSta) {
  cfg = &settings;
  id = nodeId;
  saved = false;
  error = "";
  lastActivity = millis();

  String suffix = nodeId.substring(nodeId.length() - 6);
  suffix.toUpperCase();
  const String apName = "Murmur-" + suffix;

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(apName.c_str());
  delay(100);
  Serial.printf("[portal] open: join \"%s\" and browse to http://%s\n", apName.c_str(),
                WiFi.softAPIP().toString().c_str());

  dns = new DNSServer();
  dns->setErrorReplyCode(DNSReplyCode::NoError);
  dns->start(53, "*", WiFi.softAPIP());

  server = new WebServer(80);
  server->on("/", HTTP_GET, handleRoot);
  server->on("/save", HTTP_POST, handleSave);
  server->on("/scan", HTTP_GET, handleScan);
  // OS captive-portal probes -> our page
  for (const char *probe : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt",
                            "/fwlink", "/redirect", "/canonical.html"}) {
    server->on(probe, redirectHome);
  }
  server->onNotFound(redirectHome);
  server->begin();

  scanNetworks();
  led.setMode(StatusLed::Mode::Portal);

  Result result = Result::Timeout;
  uint32_t lastScan = millis(), lastStaTry = millis(), savedAt = 0;
  while (true) {
    dns->processNextRequest();
    server->handleClient();
    led.update();
    const uint32_t now = millis();

    if (saved) {
      if (!savedAt) savedAt = now;
      if (now - savedAt > 1500) { result = Result::Saved; break; }
    }
    if (now - lastScan > 15000) {
      lastScan = now;
      scanNetworks();
    }
    const bool phoneConnected = WiFi.softAPgetStationNum() > 0;
    if (phoneConnected) lastActivity = now;  // idle timeout only runs while nobody is connected
    if (keepTryingSta && !phoneConnected && !saved) {
      if (WiFi.status() == WL_CONNECTED) { result = Result::StaConnected; break; }
      if (now - lastStaTry > 30000) {
        lastStaTry = now;
        WiFi.begin(settings.ssid.c_str(), settings.pass.c_str());
      }
    }
    if (idleTimeoutMs && !saved && now - lastActivity > idleTimeoutMs) break;
    delay(2);
  }

  server->stop();
  dns->stop();
  delete server;
  delete dns;
  server = nullptr;
  dns = nullptr;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  return result;
}

}  // namespace portal
