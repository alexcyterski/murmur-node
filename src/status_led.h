#pragma once
#include <Arduino.h>

#ifndef MURMUR_LED_PIN
#define MURMUR_LED_PIN 8
#endif
#ifndef MURMUR_LED_ACTIVE_LOW
#define MURMUR_LED_ACTIVE_LOW 1
#endif

// Non-blocking LED patterns. Call update() from loop().
class StatusLed {
 public:
  enum class Mode : uint8_t { Off, Connecting, Portal, Error, Solid };

  void begin() {
    pinMode(MURMUR_LED_PIN, OUTPUT);
    write(false);
  }
  void setMode(Mode m) { mode_ = m; }
  void setEnabled(bool en) { enabled_ = en; }
  void blip() { if (enabled_) blipUntil_ = millis() + 35; }
  void identify(uint32_t ms) { identifyUntil_ = millis() + ms; }
  void off() { mode_ = Mode::Off; blipUntil_ = identifyUntil_ = 0; write(false); }

  void update() {
    const uint32_t now = millis();
    bool on = false;
    if ((int32_t)(identifyUntil_ - now) > 0) {
      on = (now / 80) % 2;
    } else {
      switch (mode_) {
        case Mode::Connecting: on = (now / 120) % 2; break;
        case Mode::Portal:     on = (now % 1600) < 800; break;
        case Mode::Error:      on = (now % 2000) < 90 || ((now % 2000) > 220 && (now % 2000) < 310); break;
        case Mode::Solid:      on = true; break;
        case Mode::Off:        on = (int32_t)(blipUntil_ - now) > 0; break;
      }
    }
    write(on);
  }

 private:
  void write(bool on) { digitalWrite(MURMUR_LED_PIN, (on ^ MURMUR_LED_ACTIVE_LOW) ? HIGH : LOW); }

  Mode mode_ = Mode::Off;
  bool enabled_ = true;
  uint32_t blipUntil_ = 0;
  uint32_t identifyUntil_ = 0;
};
