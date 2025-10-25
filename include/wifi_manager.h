#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include "data_channel.h"

using ApStateCallback = void (*)(bool active);

void initWifiManager(ApStateCallback callback);
void updateWifiManager();
void wifiManagerHandleBleRequest(bool enable);
bool wifiManagerIsApActive();
bool wifiManagerIsConnected();
bool wifiManagerHasCredentials();
void wifiManagerSetGnssStreamingEnabled(bool enabled);

NavDataPublisher *wifiManagerNavPublisher();
SystemStatusPublisher *wifiManagerStatusPublisher();

#endif
