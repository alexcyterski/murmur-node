#include "sensors.h"

#include <Wire.h>

static constexpr uint8_t kAhtAddr = 0x38;
static constexpr uint8_t kBmp280Id = 0x58;
static constexpr uint8_t kBme280Id = 0x60;

bool SensorHub::probe(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

uint8_t SensorHub::chipId(uint8_t addr) {
  if (!probe(addr)) return 0;
  Wire.beginTransmission(addr);
  Wire.write(0xD0);  // BMx280 chip-id register
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return 0;
  return Wire.read();
}

bool SensorHub::begin(SensorType type, uint8_t pinData, uint8_t pinSda, uint8_t pinScl) {
  requested_ = type;
  type_ = type;
  pinData_ = pinData;
  pinSda_ = pinSda;
  pinScl_ = pinScl;
  ready_ = false;
  err_ = nullptr;

  if (type == SensorType::Dht22 || type == SensorType::Dht11) {
    ready_ = beginDht(type);
    return ready_;
  }

  if (!wireStarted_) {
    Wire.begin(pinSda_, pinScl_);
    Wire.setClock(100000);
    wireStarted_ = true;
  }

  if (type == SensorType::Auto) {
    const bool aht = probe(kAhtAddr);
    uint8_t id = 0;
    for (uint8_t addr : {0x76, 0x77}) {
      id = chipId(addr);
      if (id == kBmp280Id || id == kBme280Id) break;
      id = 0;
    }
    if (id == kBme280Id) type_ = SensorType::Bme280;
    else if (aht && id == kBmp280Id) type_ = SensorType::Aht20Bmp280;
    else if (aht) type_ = SensorType::Aht20;
    else if (id == kBmp280Id) type_ = SensorType::Bmp280;
    else {
      // Nothing on I2C — fall back to a DHT on the data pin.
      ready_ = beginDht(SensorType::Dht22) || beginDht(SensorType::Dht11);
      if (!ready_) err_ = "no sensor found";
      return ready_;
    }
    Serial.printf("[sensor] auto-detected %s\n", sensorKey(type_));
  }

  ready_ = beginI2C();
  return ready_;
}

bool SensorHub::beginDht(SensorType t) {
  delete dht_;
  dht_ = new DHT(pinData_, t == SensorType::Dht22 ? DHT22 : DHT11);
  dht_->begin();
  type_ = t;
  if (requested_ != SensorType::Auto) return true;  // explicit choice: trust it, report read errors later

  // Auto: verify the sensor answers with plausible values.
  delay(2100);
  const float h = dht_->readHumidity(true);
  const float c = dht_->readTemperature(false, false);
  const bool ok = !isnan(h) && !isnan(c) && h >= 0 && h <= 100 && c > -40 && c < 80;
  if (ok) Serial.printf("[sensor] auto-detected %s on GPIO%u\n", sensorKey(t), pinData_);
  return ok;
}

bool SensorHub::beginI2C() {
  switch (type_) {
    case SensorType::Aht20Bmp280:
    case SensorType::Aht20:
      if (!aht_.begin(&Wire)) {
        err_ = "AHT20 not found";
        return false;
      }
      if (type_ == SensorType::Aht20) return true;
      [[fallthrough]];
    case SensorType::Bmp280:
      for (uint8_t addr : {0x77, 0x76}) {
        if (chipId(addr) == kBmp280Id && bmp_.begin(addr, kBmp280Id)) {
          baroAddr_ = addr;
          // Forced mode: sample on demand, minimal self-heating.
          bmp_.setSampling(Adafruit_BMP280::MODE_FORCED, Adafruit_BMP280::SAMPLING_X2,
                           Adafruit_BMP280::SAMPLING_X16, Adafruit_BMP280::FILTER_X2,
                           Adafruit_BMP280::STANDBY_MS_1);
          return true;
        }
      }
      err_ = "BMP280 not found";
      return false;
    case SensorType::Bme280:
      for (uint8_t addr : {0x76, 0x77}) {
        if (chipId(addr) == kBme280Id && bme_.begin(addr, &Wire)) {
          baroAddr_ = addr;
          bme_.setSampling(Adafruit_BME280::MODE_FORCED, Adafruit_BME280::SAMPLING_X1,
                           Adafruit_BME280::SAMPLING_X4, Adafruit_BME280::SAMPLING_X1,
                           Adafruit_BME280::FILTER_OFF);
          return true;
        }
      }
      err_ = "BME280 not found";
      return false;
    default:
      err_ = "unsupported sensor";
      return false;
  }
}

bool SensorHub::read(Reading &out) {
  out = Reading{};
  if (!ready_) return false;
  err_ = nullptr;

  switch (type_) {
    case SensorType::Dht22:
    case SensorType::Dht11: {
      out.h = dht_->readHumidity();
      out.t = dht_->readTemperature();
      if (isnan(out.h) || isnan(out.t)) {
        out = Reading{};
        err_ = "DHT read failed";
      }
      break;
    }
    case SensorType::Aht20:
    case SensorType::Aht20Bmp280: {
      sensors_event_t hum, temp;
      if (aht_.getEvent(&hum, &temp)) {
        out.t = temp.temperature;
        out.h = hum.relative_humidity;
      } else {
        err_ = "AHT20 read failed";
      }
      if (type_ == SensorType::Aht20Bmp280) {
        if (bmp_.takeForcedMeasurement()) out.p = bmp_.readPressure() / 100.0f;
        else if (!err_) err_ = "BMP280 read failed";
      }
      break;
    }
    case SensorType::Bmp280:
      if (bmp_.takeForcedMeasurement()) {
        out.t = bmp_.readTemperature();
        out.p = bmp_.readPressure() / 100.0f;
      } else {
        err_ = "BMP280 read failed";
      }
      break;
    case SensorType::Bme280:
      if (bme_.takeForcedMeasurement()) {
        out.t = bme_.readTemperature();
        out.h = bme_.readHumidity();
        out.p = bme_.readPressure() / 100.0f;
      } else {
        err_ = "BME280 read failed";
      }
      break;
    default:
      err_ = "no sensor";
  }
  return out.any();
}
