// Host-side contract checks; no ESP32 or Arduino runtime required.
#include "../src/protocol.h"
#include "../src/battery.h"

#include <cassert>
#include <string>

int main() {
  assert(murmur::namePrefixBytes("") == 0);
  assert(murmur::namePrefixBytes("Bedroom") == 7);
  for (const std::string &character : {std::string("A"), std::string(u8"é"), std::string(u8"🐦")}) {
    std::string name;
    for (size_t i = 0; i < 48; ++i) name += character;
    assert(murmur::namePrefixBytes(name.c_str()) == name.size());
    const auto expected = name;
    name += character;
    assert(name.substr(0, murmur::namePrefixBytes(name.c_str())) == expected);
  }
  const std::string mixed = std::string(47, 'a') + u8"🐦" + "z";
  assert(murmur::namePrefixBytes(mixed.c_str()) == 51);

  // A timer wake waits for retained config and a successful report before sleep.
  BatteryCycle cycle;
  cycle.begin(0, false);
  cycle.connected(1000);
  assert(!cycle.configReady(2499));
  assert(cycle.configReady(2500));
  assert(!cycle.readyToSleep(2500));
  cycle.reported(2500);
  assert(!cycle.readyToSleep(2749));
  assert(cycle.readyToSleep(2750));

  // Losing the broker invalidates delivery state; reconnect needs a new report.
  cycle.disconnected();
  assert(!cycle.readyToSleep(3000));
  cycle.connected(4000);
  assert(!cycle.readyToSleep(6000));
  assert(!cycle.expired(59999));
  assert(cycle.expired(60000));
  assert(BatteryCycle::sleepSeconds(30, false) == 300);
  assert(BatteryCycle::sleepSeconds(900, false) == 900);
  assert(BatteryCycle::sleepSeconds(300, true) == 300);

  // RESET holds the device awake; BOOT extends the maintenance window.
  cycle.begin(1000, true);
  cycle.connected(2000);
  cycle.reported(4000);
  assert(!cycle.readyToSleep(70000));
  assert(!cycle.expired(70000));
  cycle.holdAwake(100000);
  assert(cycle.maintenance(399999));
  assert(!cycle.maintenance(400000));
  assert(cycle.readyToSleep(400000));

  // Millis rollover must not trigger immediate sleep or an endless awake cycle.
  const uint32_t start = UINT32_MAX - 1000;
  cycle.begin(start, false);
  cycle.connected(start);
  assert(cycle.configReady(start + 1500));
  cycle.reported(start + 1500);
  assert(cycle.readyToSleep(start + 1750));
  assert(!cycle.expired(start + 59999));
  assert(cycle.expired(start + 60000));
  cycle.begin(start, true);
  assert(cycle.maintenance(start + 299999));
  assert(!cycle.maintenance(start + 300000));
}
