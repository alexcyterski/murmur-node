#include "settings.h"
#include "protocol.h"

#include <Preferences.h>

#if __has_include("murmur_secrets.h")
#include "murmur_secrets.h"
#endif
#ifndef MURMUR_WIFI_SSID
#define MURMUR_WIFI_SSID ""
#endif
#ifndef MURMUR_WIFI_PASS
#define MURMUR_WIFI_PASS ""
#endif
#ifndef MURMUR_MQTT_HOST
#define MURMUR_MQTT_HOST ""
#endif
#ifndef MURMUR_MQTT_PORT
#define MURMUR_MQTT_PORT 1883
#endif
#ifndef MURMUR_MQTT_USER
#define MURMUR_MQTT_USER ""
#endif
#ifndef MURMUR_MQTT_PASS
#define MURMUR_MQTT_PASS ""
#endif
#ifndef MURMUR_SENSOR
#define MURMUR_SENSOR "auto"
#endif
#ifndef MURMUR_INTERVAL
#define MURMUR_INTERVAL 30
#endif
#ifndef MURMUR_BATTERY
#define MURMUR_BATTERY false
#endif

static const char *kNs = "murmur";

static const char *const kKeys[] = {"auto", "dht22", "dht11", "aht20+bmp280", "aht20", "bmp280", "bme280"};
static const char *const kLabels[] = {"Auto-detect (I2C)", "DHT22 / AM2302", "DHT11", "AHT20 + BMP280",
                                      "AHT20 / AHT10", "BMP280", "BME280"};

const char *sensorKey(SensorType t) { return kKeys[static_cast<uint8_t>(t)]; }
const char *sensorLabel(SensorType t) { return kLabels[static_cast<uint8_t>(t)]; }

SensorType sensorFromKey(const String &key) {
  for (uint8_t i = 0; i < kSensorTypeCount; i++) {
    if (key.equalsIgnoreCase(kKeys[i])) return static_cast<SensorType>(i);
  }
  return SensorType::Auto;
}

bool pinAllowed(int pin) {
  // ESP32-C3 SuperMini broken-out GPIOs, minus strapping pins 2/8/9.
  return (pin >= 0 && pin <= 10 && pin != 2 && pin != 8 && pin != 9) || pin == 20 || pin == 21;
}

void Settings::load(const String &nodeId) {
  Preferences p;
  const bool exists = p.begin(kNs, true);
  configured = exists && p.getBool("cfg", false);
  name = exists ? p.getString("name", nodeId) : nodeId;
  ssid = exists ? p.getString("ssid", MURMUR_WIFI_SSID) : MURMUR_WIFI_SSID;
  pass = exists ? p.getString("pass", MURMUR_WIFI_PASS) : MURMUR_WIFI_PASS;
  mqttHost = exists ? p.getString("mhost", MURMUR_MQTT_HOST) : MURMUR_MQTT_HOST;
  mqttPort = exists ? p.getUShort("mport", MURMUR_MQTT_PORT) : MURMUR_MQTT_PORT;
  mqttUser = exists ? p.getString("muser", MURMUR_MQTT_USER) : MURMUR_MQTT_USER;
  mqttPass = exists ? p.getString("mpass", MURMUR_MQTT_PASS) : MURMUR_MQTT_PASS;
  baseTopic = exists ? p.getString("base", "murmur") : "murmur";
  otaPass = exists ? p.getString("ota", "") : "";
  sensor = static_cast<SensorType>(exists ? p.getUChar("sensor", (uint8_t)sensorFromKey(MURMUR_SENSOR))
                                          : (uint8_t)sensorFromKey(MURMUR_SENSOR));
  pinData = exists ? p.getUChar("pdata", 4) : 4;
  pinSda = exists ? p.getUChar("psda", 5) : 5;
  pinScl = exists ? p.getUChar("pscl", 6) : 6;
  interval = exists ? p.getUInt("intv", MURMUR_INTERVAL) : MURMUR_INTERVAL;
  led = exists ? p.getBool("led", true) : true;
  battery = exists ? p.getBool("battery", MURMUR_BATTERY) : MURMUR_BATTERY;
  lowTx = exists ? p.getBool("lowtx", false) : false;
  if (exists) p.end();

  if ((uint8_t)sensor >= kSensorTypeCount) sensor = SensorType::Auto;
  name = name.substring(0, murmur::namePrefixBytes(name.c_str()));
  interval = constrain(interval, kIntervalMin, kIntervalMax);
  baseTopic.trim();
  while (baseTopic.startsWith("/")) baseTopic.remove(0, 1);
  while (baseTopic.endsWith("/")) baseTopic.remove(baseTopic.length() - 1);
  if (baseTopic.isEmpty()) baseTopic = "murmur";

  // Credentials compiled in via murmur_secrets.h count as configured.
  if (!configured && ssid.length() && mqttHost.length()) configured = true;
}

void Settings::save() {
  Preferences p;
  p.begin(kNs, false);
  p.putBool("cfg", configured);
  p.putString("name", name);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.putString("mhost", mqttHost);
  p.putUShort("mport", mqttPort);
  p.putString("muser", mqttUser);
  p.putString("mpass", mqttPass);
  p.putString("base", baseTopic);
  p.putString("ota", otaPass);
  p.putUChar("sensor", (uint8_t)sensor);
  p.putUChar("pdata", pinData);
  p.putUChar("psda", pinSda);
  p.putUChar("pscl", pinScl);
  p.putUInt("intv", interval);
  p.putBool("led", led);
  p.putBool("battery", battery);
  p.putBool("lowtx", lowTx);
  p.end();
}

void Settings::erase() {
  Preferences p;
  p.begin(kNs, false);
  p.clear();
  p.end();
}
