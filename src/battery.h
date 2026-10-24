#pragma once
#include <stdint.h>

// Timing only: kept independent of Arduino so wake/failure behavior is testable.
class BatteryCycle {
 public:
  static constexpr uint32_t kWakeBudgetMs = 60000;
  static constexpr uint32_t kMaintenanceMs = 300000;
  static constexpr uint32_t kConfigWaitMs = 1500;
  static constexpr uint32_t kDrainMs = 250;

  void begin(uint32_t now, bool maintenance) {
    started_ = now;
    maintenance_ = maintenance;
    maintenanceUntil_ = now + kMaintenanceMs;
    connected_ = sent_ = false;
  }
  void holdAwake(uint32_t now) {
    maintenance_ = true;
    maintenanceUntil_ = now + kMaintenanceMs;
  }
  bool maintenance(uint32_t now) const {
    return maintenance_ && (int32_t)(maintenanceUntil_ - now) > 0;
  }
  void connected(uint32_t now) {
    connected_ = true;
    connectedAt_ = now;
    sent_ = false;
  }
  void disconnected() { connected_ = sent_ = false; }
  bool configReady(uint32_t now) const {
    return connected_ && now - connectedAt_ >= kConfigWaitMs;
  }
  void reported(uint32_t now) { sent_ = true; sentAt_ = now; }
  bool readyToSleep(uint32_t now) const {
    return !maintenance(now) && configReady(now) && sent_ && now - sentAt_ >= kDrainMs;
  }
  bool expired(uint32_t now) const {
    return !maintenance(now) && now - started_ >= kWakeBudgetMs;
  }
  static uint32_t sleepSeconds(uint32_t interval, bool delivered) {
    return !delivered && interval < 300 ? 300 : interval;
  }

 private:
  uint32_t started_ = 0, maintenanceUntil_ = 0, connectedAt_ = 0, sentAt_ = 0;
  bool maintenance_ = false, connected_ = false, sent_ = false;
};
