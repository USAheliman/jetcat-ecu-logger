#pragma once
// WiFi (home network, for the browser and OTA from PlatformIO) and Bluetooth
// (field), both answering the same API through ble_bridge.h's ReqRouter.
#include <Arduino.h>

namespace net {
void begin();                 // WiFi from saved credentials, mDNS, routes, Bluetooth
void poll();                  // call from loop() and while waiting on the ECU
void setWifi(const String& ssid, const String& pass);   // saved in NVS; empty ssid = WiFi off
String wifiStatus();          // short text for the console
String ip();
bool bleConnected();        // a phone is connected over Bluetooth
bool otaBusy();               // a firmware update is being written
void markFirmwareGood();      // confirms a freshly updated firmware (see rollback note in net.cpp)
}
