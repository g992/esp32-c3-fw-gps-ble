#include "firmware_app.h"

#include "gps_ble.h"
#include "gps_controller.h"
#include "led_status.h"
#include "logger.h"
#include "system_mode.h"
#include "wifi_manager.h"

#include <Arduino.h>
#include <esp_system.h>

FirmwareApp &firmwareApp() {
  static FirmwareApp app;
  return app;
}

void FirmwareApp::begin() {
  Serial.begin(115200);
  initSystemMode();
  logPrintln("[sys] Booting firmware...");

  gpsController().begin();

  initBLE();
  configurePublishers();
  initStatusLED();
  initWifiManager(onWifiApStateChanged);
  updateApControlCharacteristic(wifiManagerIsApActive());

  logPrintln("[sys] Boot complete.");
}

void FirmwareApp::tick() {
  updateWifiManager();
  gpsController().loop();
  updateStatusLED();
  bleTick();
  processPendingRestart();
}

void FirmwareApp::configurePublishers() {
  gpsController().addNavPublisher(bleNavPublisher());
  gpsController().addStatusPublisher(bleStatusPublisher());
  gpsController().addNavPublisher(wifiManagerNavPublisher());
  gpsController().addStatusPublisher(wifiManagerStatusPublisher());
}

void FirmwareApp::onWifiApStateChanged(bool active) {
  updateApControlCharacteristic(active);
}

void FirmwareApp::processPendingRestart() {
  if (!restartPending)
    return;
  if (millis() - restartRequestedAt < 200)
    return;
  logPrintln("[sys] Restarting now...");
  Serial.println("[sys] Restarting now...");
  Serial.flush();
  delay(50);
  esp_restart();
}
