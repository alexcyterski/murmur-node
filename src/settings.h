#pragma once
#include <Arduino.h>

// Note: the DHT library #defines DHT11/DHT22, hence the CamelCase names.
enum class SensorType : uint8_t { Auto = 0, Dht22, Dht11, Aht20Bmp280, Aht20, Bmp280, Bme280 };

const char *sensorKey(SensorType t);
const char *sensorLabel(SensorType t);
SensorType sensorFromKey(const String &key);
constexpr uint8_t kSensorTypeCount = 7;

constexpr uint32_t kIntervalMin = 2;
constexpr uint32_t kIntervalMax = 3600;

bool pinAllowed(int pin);

struct Settings {
  String name;
  String ssid, pass;
  String mqttHost, mqttUser, mqttPass, baseTopic;
  uint16_t mqttPort = 1883;
  String otaPass;
  SensorType sensor = SensorType::Auto;
  uint8_t pinData = 4;
  uint8_t pinSda = 5;
  uint8_t pinScl = 6;
  uint32_t interval = 30;
  bool led = true;
  bool lowTx = false;      // reduced Wi-Fi TX power, fixes some SuperMini boards
  bool configured = false;

  void load(const String &nodeId);
  void save();
  static void erase();
};
