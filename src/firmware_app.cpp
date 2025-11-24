#include "firmware_app.h"

#include "gps_ble.h"
#include "gps_controller.h"
#include "led_status.h"
#include "logger.h"
#include "ota_service.h"
#include "system_mode.h"
#include "wifi_manager.h"

#include <Arduino.h>
#include <esp_system.h>

FirmwareApp &firmwareApp() {
  static FirmwareApp app;
  return app;
}

void FirmwareApp::begin() {
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);
  Serial.begin(115200);
  initSystemMode();
  logPrintln("[sys] Booting firmware...");

  gpsController().begin();

  initBLE();
  configurePublishers();
  initStatusLED();
  initModeLED();
  initWifiManager(onWifiApStateChanged);
  updateApControlCharacteristic(wifiManagerIsApActive());

  logPrintln("[sys] Boot complete.");
}

void FirmwareApp::tick() {
  updateWifiManager();
  otaTick();
  gpsController().loop();
  updateStatusLED();
  updateModeLED(isSerialPassthroughMode(), otaUpdateInProgress(),
                wifiManagerIsConnected());
  bleTick();
  processPendingRestart();
}

void FirmwareApp::requestRestart(const char *reason) {
  if (restartPending)
    return;
  restartPending = true;
  restartReason = reason;
  restartRequestedAt = millis();
  if (reason) {
    logPrintf("[sys] Restart requested (%s)\n", reason);
  } else {
    logPrintln("[sys] Restart requested");
  }
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
  if (restartReason) {
    logPrintf("[sys] Restarting now (%s)\n", restartReason);
    Serial.print("[sys] Restarting now (");
    Serial.print(restartReason);
    Serial.println(")");
  } else {
    logPrintln("[sys] Restarting now...");
    Serial.println("[sys] Restarting now...");
  }
  Serial.flush();
  delay(50);
  esp_restart();
}
