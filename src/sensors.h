#pragma once
#include <Adafruit_AHTX0.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <Arduino.h>
#include <DHT.h>

#include "settings.h"

struct Reading {
  float t = NAN;  // °C
  float h = NAN;  // %RH
  float p = NAN;  // hPa
  bool any() const { return !isnan(t) || !isnan(h) || !isnan(p); }
};

class SensorHub {
 public:
  // Initialise the configured sensor. With SensorType::Auto the I2C bus is
  // probed (AHT20 @0x38, BMP280/BME280 @0x76/0x77), then DHT22/DHT11 on the data pin.
  bool begin(SensorType type, uint8_t pinData, uint8_t pinSda, uint8_t pinScl);
  bool read(Reading &out);
  bool ready() const { return ready_; }
  SensorType resolved() const { return type_; }
  const char *error() const { return err_; }

 private:
  bool beginI2C();
  bool beginDht(SensorType t);
  bool probe(uint8_t addr);
  uint8_t chipId(uint8_t addr);

  SensorType requested_ = SensorType::Auto;
  SensorType type_ = SensorType::Auto;
  uint8_t pinData_ = 4, pinSda_ = 5, pinScl_ = 6;
  bool wireStarted_ = false;
  bool ready_ = false;
  const char *err_ = nullptr;

  DHT *dht_ = nullptr;
  Adafruit_AHTX0 aht_;
  Adafruit_BMP280 bmp_{&Wire};
  Adafruit_BME280 bme_;
  uint8_t baroAddr_ = 0;
};
