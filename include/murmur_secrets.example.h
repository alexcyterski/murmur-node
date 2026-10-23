// Optional: bake credentials into the firmware so freshly flashed nodes come
// up without the setup portal. Handy when flashing a batch of boards.
//
//   cp include/murmur_secrets.example.h include/murmur_secrets.h
//
// murmur_secrets.h is git-ignored. Anything left as "" falls back to the
// portal. Values saved through the portal always override these.
#pragma once

#define MURMUR_WIFI_SSID     "MyHomeWiFi"
#define MURMUR_WIFI_PASS     "correct horse battery staple"

#define MURMUR_MQTT_HOST     "raspberrypi.local"   // IP or mDNS name of the Pi
#define MURMUR_MQTT_PORT     1883
#define MURMUR_MQTT_USER     ""
#define MURMUR_MQTT_PASS     ""

// auto | dht22 | dht11 | aht20+bmp280 | aht20 | bmp280 | bme280
#define MURMUR_SENSOR        "auto"
#define MURMUR_INTERVAL      30                    // seconds between reports
