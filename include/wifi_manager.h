#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>

using ApStateCallback = void (*)(bool active);

void initWifiManager(ApStateCallback callback);
void updateWifiManager();
void wifiManagerHandleBleRequest(bool enable);
bool wifiManagerIsApActive();
bool wifiManagerIsConnected();
bool wifiManagerHasCredentials();
void wifiManagerUpdateNavSnapshot(float latitude, float longitude,
                                  float heading, float speed, float altitude);
void wifiManagerUpdateStatusSnapshot(uint8_t fix, float hdop,
                                     const String &signalsJson,
                                     int32_t ttffSeconds,
                                     uint8_t satellites);
void wifiManagerSetGnssStreamingEnabled(bool enabled);

#endif
