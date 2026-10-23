#pragma once
#include <Arduino.h>

#include "settings.h"
#include "status_led.h"

// Captive setup portal: opens a "Murmur-XXXXXX" access point serving a form
// for sensor type, Wi-Fi and MQTT settings.
namespace portal {

enum class Result : uint8_t { Saved, Timeout, StaConnected };

// Blocks until the form is saved, the portal sits idle for idleTimeoutMs
// (0 = never), or - when keepTryingSta is set - the station link comes up
// while nobody is using the portal.
Result run(Settings &settings, const String &nodeId, StatusLed &led, uint32_t idleTimeoutMs,
           bool keepTryingSta);

}  // namespace portal
