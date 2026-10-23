// Murmur node — ESP32-C3 SuperMini climate sensor reporting over MQTT.
//
// Topics (base defaults to "murmur"):
//   <base>/<id>/state   out  {"t":21.4,"h":44.1,"p":1012.6,"rssi":-58,"up":3600,"heap":181234}
//   <base>/<id>/status  out  "online" / "offline" (retained, last will)
//   <base>/<id>/info    out  device metadata (retained)
//   <base>/<id>/config  in   {"interval":30,"led":true,"name":"Bedroom"} (retained, from the server)
//   <base>/<id>/cmd     in   {"action":"identify"|"read"|"reboot"}

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_timer.h>

#include "portal.h"
#include "sensors.h"
#include "settings.h"
#include "status_led.h"

#ifndef MURMUR_FW_VERSION
#define MURMUR_FW_VERSION "dev"
#endif
#ifndef MURMUR_BUTTON_PIN
#define MURMUR_BUTTON_PIN 9
#endif

namespace {

constexpr uint32_t kWifiConnectTimeout = 20000;
constexpr uint32_t kWifiLostPortalAfter = 180000;  // open the portal after 3 min without Wi-Fi
constexpr uint32_t kPortalIdleTimeout = 300000;
constexpr uint32_t kFactoryResetHold = 5000;
constexpr uint32_t kSensorRetry = 30000;

Settings settings;
SensorHub sensor;
StatusLed led;
WiFiClient net;
PubSubClient mqtt(net);

String nodeId;
String topicState, topicStatus, topicInfo, topicConfig, topicCmd;
IPAddress brokerIp;

uint32_t lastReport = 0;
bool reportNow = true;
bool infoDirty = true;
uint32_t mqttRetryAt = 0;
uint32_t mqttBackoff = 2000;
uint32_t wifiLostAt = 0;
uint32_t sensorRetryAt = 0;
uint32_t rebootAt = 0;
uint32_t buttonDownAt = 0;

inline bool due(uint32_t at) { return (int32_t)(millis() - at) >= 0; }
inline uint32_t uptimeSec() { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }

String makeNodeId() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[16];
  snprintf(buf, sizeof buf, "murmur-%02x%02x%02x", mac[3], mac[4], mac[5]);
  return buf;
}

// ---------------------------------------------------------------------------
// Button: short press = report now, hold 5 s = factory reset
// ---------------------------------------------------------------------------
void handleButton() {
  const bool down = digitalRead(MURMUR_BUTTON_PIN) == LOW;
  const uint32_t now = millis();
  if (down && !buttonDownAt) {
    buttonDownAt = now;
  } else if (down && now - buttonDownAt >= kFactoryResetHold) {
    Serial.println("[button] factory reset");
    led.setMode(StatusLed::Mode::Solid);
    led.update();
    if (mqtt.connected()) {
      mqtt.publish(topicStatus.c_str(), "offline", true);
      mqtt.disconnect();
    }
    Settings::erase();
    delay(400);
    ESP.restart();
  } else if (!down && buttonDownAt) {
    if (now - buttonDownAt > 40) {  // debounced short press
      reportNow = true;
      led.identify(400);
    }
    buttonDownAt = 0;
  }
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
bool connectWifi() {
  WiFi.setHostname(nodeId.c_str());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  led.setMode(StatusLed::Mode::Connecting);

  // Some SuperMini boards can't associate at full TX power. If the first
  // attempt fails, retry at reduced power and remember it if that works.
  for (int attempt = 0; attempt < 2; attempt++) {
    const bool lowTx = settings.lowTx || attempt == 1;
    Serial.printf("[wifi] connecting to \"%s\"%s\n", settings.ssid.c_str(), lowTx ? " (reduced TX power)" : "");
    WiFi.begin(settings.ssid.c_str(), settings.pass.c_str());
    if (lowTx) WiFi.setTxPower(WIFI_POWER_8_5dBm);
    const uint32_t start = millis();
    while (millis() - start < kWifiConnectTimeout) {
      if (WiFi.status() == WL_CONNECTED) {
        if (lowTx && !settings.lowTx) {
          settings.lowTx = true;
          settings.save();
          Serial.println("[wifi] reduced TX power works on this board; saved");
        }
        return true;
      }
      handleButton();
      led.update();
      delay(20);
    }
    if (settings.lowTx) break;
    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_STA);
  }
  return false;
}

void openPortalOrRestart() {
  Serial.println("[wifi] can't connect - opening setup portal");
  if (mqtt.connected()) mqtt.disconnect();
  if (portal::run(settings, nodeId, led, kPortalIdleTimeout, true) != portal::Result::StaConnected) {
    ESP.restart();
  }
  Serial.println("[wifi] reconnected, portal closed");
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
void publishJson(const String &topic, JsonDocument &doc, bool retain) {
  char buf[512];
  const size_t n = serializeJson(doc, buf, sizeof buf);
  mqtt.publish(topic.c_str(), reinterpret_cast<const uint8_t *>(buf), n, retain);
}

void publishInfo() {
  JsonDocument doc;
  doc["id"] = nodeId;
  doc["name"] = settings.name;
  doc["sensor"] = sensorKey(sensor.ready() ? sensor.resolved() : settings.sensor);
  doc["fw"] = MURMUR_FW_VERSION;
  doc["model"] = "ESP32-C3";
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["ssid"] = WiFi.SSID();
  doc["interval"] = settings.interval;
  doc["led"] = settings.led;
  publishJson(topicInfo, doc, true);
  infoDirty = false;
}

void publishState() {
  Reading r;
  const bool ok = sensor.read(r);
  JsonDocument doc;
  if (!isnan(r.t)) doc["t"] = serialized(String(r.t, 2));
  if (!isnan(r.h)) doc["h"] = serialized(String(r.h, 2));
  if (!isnan(r.p)) doc["p"] = serialized(String(r.p, 2));
  doc["rssi"] = WiFi.RSSI();
  doc["up"] = uptimeSec();
  doc["heap"] = ESP.getFreeHeap();
  if (!ok || sensor.error()) doc["err"] = sensor.error() ? sensor.error() : "sensor not ready";
  publishJson(topicState, doc, false);
  led.blip();
  Serial.printf("[report] t=%.2f h=%.2f p=%.2f%s%s\n", r.t, r.h, r.p, sensor.error() ? " err=" : "",
                sensor.error() ? sensor.error() : "");
}

void onMessage(char *topic, byte *payload, unsigned int len) {
  if (len == 0) return;  // retained message cleared
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;

  if (topicConfig == topic) {
    bool changed = false;
    if (doc["interval"].is<int>()) {
      const uint32_t iv = constrain(doc["interval"].as<long>(), (long)kIntervalMin, (long)kIntervalMax);
      if (iv != settings.interval) {
        settings.interval = iv;
        changed = true;
      }
    }
    if (doc["led"].is<bool>() && doc["led"].as<bool>() != settings.led) {
      settings.led = doc["led"].as<bool>();
      led.setEnabled(settings.led);
      changed = true;
    }
    if (doc["name"].is<const char *>()) {
      String name = doc["name"].as<const char *>();
      name = name.substring(0, 32);
      if (name.length() && name != settings.name) {
        settings.name = name;
        changed = true;
      }
    }
    if (changed) {
      settings.save();
      infoDirty = true;
      Serial.printf("[config] interval=%lus led=%d name=\"%s\"\n", (unsigned long)settings.interval, settings.led,
                    settings.name.c_str());
    }
  } else if (topicCmd == topic) {
    const String action = doc["action"] | "";
    Serial.printf("[cmd] %s\n", action.c_str());
    if (action == "identify") led.identify(10000);
    else if (action == "read") reportNow = true;
    else if (action == "reboot") rebootAt = millis() + 300;
  }
}

void configureBroker() {
  const String &host = settings.mqttHost;
  if (host.endsWith(".local")) {
    // The ESP32 resolver doesn't do mDNS; ask the responder directly.
    const IPAddress ip = MDNS.queryHost(host.substring(0, host.length() - 6), 2000);
    if (ip.toString() != "0.0.0.0") {
      brokerIp = ip;
      mqtt.setServer(brokerIp, settings.mqttPort);
      return;
    }
    Serial.printf("[mqtt] mDNS lookup for %s failed\n", host.c_str());
  }
  mqtt.setServer(host.c_str(), settings.mqttPort);
}

void mqttConnect() {
  configureBroker();
  const char *user = settings.mqttUser.length() ? settings.mqttUser.c_str() : nullptr;
  const char *pass = user && settings.mqttPass.length() ? settings.mqttPass.c_str() : nullptr;
  Serial.printf("[mqtt] connecting to %s:%u\n", settings.mqttHost.c_str(), settings.mqttPort);
  if (mqtt.connect(nodeId.c_str(), user, pass, topicStatus.c_str(), 1, true, "offline")) {
    Serial.println("[mqtt] connected");
    mqttBackoff = 2000;
    mqtt.publish(topicStatus.c_str(), "online", true);
    mqtt.subscribe(topicConfig.c_str(), 1);
    mqtt.subscribe(topicCmd.c_str(), 0);
    infoDirty = true;
    reportNow = true;
  } else {
    Serial.printf("[mqtt] failed (state %d), retry in %lus\n", mqtt.state(), (unsigned long)mqttBackoff / 1000);
    mqttRetryAt = millis() + mqttBackoff;
    mqttBackoff = min<uint32_t>(mqttBackoff * 2, 60000);
  }
}

void setupOta() {
  ArduinoOTA.setHostname(nodeId.c_str());
  if (settings.otaPass.length()) ArduinoOTA.setPassword(settings.otaPass.c_str());
  ArduinoOTA.onStart([] {
    Serial.println("[ota] update starting");
    if (mqtt.connected()) {
      mqtt.publish(topicStatus.c_str(), "offline", true);
      mqtt.disconnect();
    }
    led.setMode(StatusLed::Mode::Connecting);
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { led.update(); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %u\n", e); });
  ArduinoOTA.begin();  // also starts the mDNS responder: <id>.local
}

void updateLedMode() {
  StatusLed::Mode mode = StatusLed::Mode::Off;
  if (WiFi.status() != WL_CONNECTED) mode = StatusLed::Mode::Connecting;
  else if (!mqtt.connected() || !sensor.ready()) mode = StatusLed::Mode::Error;
  led.setMode(mode);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  const uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 1500) delay(10);  // give USB CDC a moment

  led.begin();
  pinMode(MURMUR_BUTTON_PIN, INPUT_PULLUP);
  nodeId = makeNodeId();
  Serial.printf("\n[murmur] node %s  firmware %s\n", nodeId.c_str(), MURMUR_FW_VERSION);

  settings.load(nodeId);
  led.setEnabled(settings.led);
  if (!settings.configured) {
    portal::run(settings, nodeId, led, 0, false);  // returns once saved
    ESP.restart();
  }

  const String base = settings.baseTopic + "/" + nodeId + "/";
  topicState = base + "state";
  topicStatus = base + "status";
  topicInfo = base + "info";
  topicConfig = base + "config";
  topicCmd = base + "cmd";

  Serial.printf("[sensor] %s  data=GPIO%u sda=GPIO%u scl=GPIO%u\n", sensorKey(settings.sensor), settings.pinData,
                settings.pinSda, settings.pinScl);
  if (!sensor.begin(settings.sensor, settings.pinData, settings.pinSda, settings.pinScl)) {
    Serial.printf("[sensor] init failed: %s (retrying every %lus)\n", sensor.error(),
                  (unsigned long)kSensorRetry / 1000);
    sensorRetryAt = millis() + kSensorRetry;
  }

  if (!connectWifi()) openPortalOrRestart();
  Serial.printf("[wifi] connected  ip=%s  rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

  setupOta();
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
  mqtt.setCallback(onMessage);
}

void loop() {
  handleButton();
  const uint32_t now = millis();

  if (WiFi.status() == WL_CONNECTED) {
    wifiLostAt = 0;
    ArduinoOTA.handle();
    if (!mqtt.connected() && due(mqttRetryAt)) mqttConnect();
    mqtt.loop();
  } else {
    if (!wifiLostAt) {
      wifiLostAt = now ? now : 1;
      Serial.println("[wifi] connection lost, reconnecting");
    } else if (now - wifiLostAt > kWifiLostPortalAfter) {
      openPortalOrRestart();
      wifiLostAt = 0;
    }
  }

  if (!sensor.ready() && due(sensorRetryAt)) {
    if (sensor.begin(settings.sensor, settings.pinData, settings.pinSda, settings.pinScl)) {
      Serial.printf("[sensor] %s ready\n", sensorKey(sensor.resolved()));
      infoDirty = true;
      reportNow = true;
    }
    sensorRetryAt = millis() + kSensorRetry;
  }

  if (mqtt.connected()) {
    if (infoDirty) publishInfo();
    if (reportNow || now - lastReport >= settings.interval * 1000UL) {
      lastReport = now;
      reportNow = false;
      publishState();
    }
  }

  if (rebootAt && due(rebootAt)) {
    mqtt.publish(topicStatus.c_str(), "offline", true);
    mqtt.disconnect();
    delay(100);
    ESP.restart();
  }

  updateLedMode();
  led.update();
  delay(5);
}
