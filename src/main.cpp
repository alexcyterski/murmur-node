// Murmur node — ESP32-C3 SuperMini climate sensor reporting over MQTT.
//
// Topics (base defaults to "murmur"):
//   <base>/<id>/state   out  {"t":21.4,"h":44.1,"p":1012.6,"rssi":-58,"up":3600,"heap":181234}
//   <base>/<id>/status  out  "online" / "sleeping" / "offline" (retained; will = offline)
//   <base>/<id>/info    out  device metadata (retained)
//   <base>/<id>/config  in   {"interval":300,"led":true,"name":"Bedroom","battery":true} (retained)
//   <base>/<id>/cmd     in   {"action":"identify"|"read"|"reboot"}

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#include "battery.h"
#include "portal.h"
#include "protocol.h"
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
BatteryCycle batteryCycle;
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
uint32_t sensorReadAt = 0;
bool otaBusy = false;

inline bool due(uint32_t at) { return (int32_t)(millis() - at) >= 0; }
inline uint32_t uptimeSec() { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }

String makeNodeId() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[16];
  snprintf(buf, sizeof buf, "murmur-%02x%02x%02x", mac[3], mac[4], mac[5]);
  return buf;
}

void sleepNow(bool delivered) {
  const uint32_t seconds = BatteryCycle::sleepSeconds(settings.interval, delivered);
  Serial.printf("[battery] sleeping for %lus%s\n", (unsigned long)seconds, delivered ? "" : " (retry)");
  if (mqtt.connected()) {
    mqtt.publish(topicStatus.c_str(), delivered ? "sleeping" : "offline", true);
    delay(100);  // allow the final status packet onto the wire before disconnect
    mqtt.disconnect();  // graceful disconnect suppresses the offline last will
    delay(150);  // let DISCONNECT leave the radio before Wi-Fi goes off, or the broker fires the will
  }
  WiFi.mode(WIFI_OFF);
  led.off();
  gpio_hold_en((gpio_num_t)MURMUR_LED_PIN);
  gpio_deep_sleep_hold_en();
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}

// ---------------------------------------------------------------------------
// BOOT while awake: report now + battery maintenance window; hold 5 s = reset
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
      if (settings.battery) {
        batteryCycle.holdAwake(now);
        Serial.println("[battery] maintenance window: 5 minutes");
      }
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
  const auto result = portal::run(settings, nodeId, led, kPortalIdleTimeout, true);
  if (result == portal::Result::Timeout && settings.battery) sleepNow(false);
  if (result != portal::Result::StaConnected) {
    ESP.restart();
  }
  Serial.println("[wifi] reconnected, portal closed");
}

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------
bool publishJson(const String &topic, JsonDocument &doc, bool retain) {
  char buf[1024];
  if (measureJson(doc) >= sizeof buf) return false;
  const size_t n = serializeJson(doc, buf, sizeof buf);
  return mqtt.publish(topic.c_str(), reinterpret_cast<const uint8_t *>(buf), n, retain);
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
  doc["battery"] = settings.battery;
  infoDirty = !publishJson(topicInfo, doc, true);
}

bool publishState() {
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
  const bool sent = publishJson(topicState, doc, false);
  if (sent) led.blip();
  Serial.printf("[report] t=%.2f h=%.2f p=%.2f%s%s\n", r.t, r.h, r.p, sensor.error() ? " err=" : "",
                 sensor.error() ? sensor.error() : "");
  return sent;
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
    if (doc["battery"].is<bool>() && doc["battery"].as<bool>() != settings.battery) {
      settings.battery = doc["battery"].as<bool>();
      batteryCycle.begin(millis(), false);
      batteryCycle.connected(millis());
      changed = true;
    }
    if (doc["name"].is<const char *>()) {
      String name = doc["name"].as<const char *>();
      name = name.substring(0, murmur::namePrefixBytes(name.c_str()));
      if (name.length() && name != settings.name) {
        settings.name = name;
        changed = true;
      }
    }
    if (changed) {
      settings.save();
      infoDirty = true;
      reportNow = true;
      Serial.printf("[config] interval=%lus led=%d name=\"%s\"\n", (unsigned long)settings.interval, settings.led,
                    settings.name.c_str());
    }
  } else if (topicCmd == topic) {
    const String action = doc["action"] | "";
    Serial.printf("[cmd] %s\n", action.c_str());
    if (action == "identify") {
      led.identify(10000);
      if (settings.battery) batteryCycle.holdAwake(millis());
    }
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
    batteryCycle.connected(millis());
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
    otaBusy = true;
    Serial.println("[ota] update starting");
    if (mqtt.connected()) {
      mqtt.publish(topicStatus.c_str(), "offline", true);
      mqtt.disconnect();
    }
    led.setMode(StatusLed::Mode::Connecting);
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { led.update(); });
  ArduinoOTA.onError([](ota_error_t e) {
    otaBusy = false;
    batteryCycle.holdAwake(millis());
    Serial.printf("[ota] error %u\n", e);
  });
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
  const bool timerWake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
  while (!timerWake && !Serial && millis() - t0 < 1500) delay(10);

  gpio_deep_sleep_hold_dis();
  gpio_hold_dis((gpio_num_t)MURMUR_LED_PIN);
  led.begin();
  pinMode(MURMUR_BUTTON_PIN, INPUT_PULLUP);
  nodeId = makeNodeId();
  Serial.printf("\n[murmur] node %s  firmware %s\n", nodeId.c_str(), MURMUR_FW_VERSION);

  settings.load(nodeId);
  batteryCycle.begin(millis(), settings.battery && !timerWake);
  if (settings.battery && !timerWake) Serial.println("[battery] maintenance window: 5 minutes");
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
  sensorReadAt = millis() + 2100;  // also allow DHT startup time on fast Wi-Fi joins

  if (!connectWifi()) {
    if (settings.battery && !batteryCycle.maintenance(millis())) sleepNow(false);
    openPortalOrRestart();
  }
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

  if (settings.battery && !otaBusy && !buttonDownAt && !rebootAt) {
    if (!infoDirty && !reportNow && batteryCycle.readyToSleep(now)) sleepNow(true);
    if (batteryCycle.expired(now)) sleepNow(false);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiLostAt = 0;
    ArduinoOTA.handle();
    if (!mqtt.connected() && due(mqttRetryAt)) mqttConnect();
    mqtt.loop();
    if (!mqtt.connected()) batteryCycle.disconnected();
  } else {
    batteryCycle.disconnected();
    if (!wifiLostAt) {
      wifiLostAt = now ? now : 1;
      Serial.println("[wifi] connection lost, reconnecting");
    } else if (!settings.battery && now - wifiLostAt > kWifiLostPortalAfter) {
      openPortalOrRestart();
      wifiLostAt = 0;
    }
  }

  if (!sensor.ready() && due(sensorRetryAt)) {
    if (sensor.begin(settings.sensor, settings.pinData, settings.pinSda, settings.pinScl)) {
      Serial.printf("[sensor] %s ready\n", sensorKey(sensor.resolved()));
      infoDirty = true;
      reportNow = true;
      sensorReadAt = millis() + 2100;
    }
    sensorRetryAt = millis() + kSensorRetry;
  }

  if (mqtt.connected()) {
    if (infoDirty) publishInfo();
    if (due(sensorReadAt) && (!settings.battery || batteryCycle.configReady(millis())) &&
        (reportNow || now - lastReport >= settings.interval * 1000UL)) {
      lastReport = now;
      reportNow = false;
      if (publishState()) batteryCycle.reported(millis());
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
