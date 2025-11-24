#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include "data_channel.h"

class WebServer;

enum class WifiConnectionState { Disconnected, Connecting, Connected };

struct WifiStatusInfo {
  WifiConnectionState state = WifiConnectionState::Disconnected;
  String ip;
  bool apActive = false;
};

using ApStateCallback = void (*)(bool active);

void initWifiManager(ApStateCallback callback);
void updateWifiManager();
void wifiManagerHandleBleRequest(bool enable);
bool wifiManagerIsApActive();
bool wifiManagerIsConnected();
String wifiManagerApSsid();
bool wifiManagerHasCredentials();
WifiStatusInfo wifiManagerGetStatus();
WebServer *wifiManagerHttpServer();
void wifiManagerSetGnssStreamingEnabled(bool enabled);

NavDataPublisher *wifiManagerNavPublisher();
SystemStatusPublisher *wifiManagerStatusPublisher();

#endif
