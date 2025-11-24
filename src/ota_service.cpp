#include "ota_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Update.h>
#include <esp_system.h>

#include <ElegantOTA.h>

#include "logger.h"
#include "wifi_manager.h"

namespace {

constexpr uint16_t kOtaHttpPort = 80;
constexpr unsigned long kOtaEnableWindowMs = 10UL * 60UL * 1000UL;
constexpr unsigned long kProgressLogIntervalMs = 1000;
constexpr unsigned long kOtaStartTimeoutMs = 25000;

NimBLECharacteristic *gToggleChar = nullptr;
ELEGANTOTA_WEBSERVER *gOtaServer = nullptr;
bool gOtaEnabled = false;
bool gOtaInProgress = false;
bool gServerRunning = false;
bool gStartedApForOta = false;
unsigned long gOtaEnableAt = 0;
unsigned long gLastProgressLog = 0;
unsigned long gOtaStartAt = 0;
bool gOtaReceivedBytes = false;

class OtaToggleCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) override;
};

OtaToggleCallbacks gToggleCallbacks;

uint8_t flagValue() { return gOtaEnabled ? '1' : '0'; }

void updateToggleCharacteristic(bool notify) {
  if (!gToggleChar)
    return;
  uint8_t value = flagValue();
  gToggleChar->setValue(&value, 1);
  if (notify) {
    gToggleChar->notify();
  }
}

bool wifiReadyForOta() {
  return wifiManagerIsConnected() || wifiManagerIsApActive();
}

void ensureApForOta() {
  if (!gOtaEnabled || wifiReadyForOta() || gStartedApForOta)
    return;
  wifiManagerHandleBleRequest(true);
  gStartedApForOta = true;
  logPrintln("[ota] Requested AP start for OTA access");
}

void stopApIfStartedForOta() {
  if (!gStartedApForOta)
    return;
  if (wifiManagerIsApActive()) {
    wifiManagerHandleBleRequest(false);
  }
  gStartedApForOta = false;
}

void logOtaEntryPoints() {
  if (wifiManagerIsConnected()) {
    logPrintf("[ota] Update page: http://%s/update\n",
              WiFi.localIP().toString().c_str());
  }
  if (wifiManagerIsApActive()) {
    logPrintf("[ota] AP update page: http://%s/update\n",
              WiFi.softAPIP().toString().c_str());
  }
}

void applyOtaAuthGuard() {
  if (!gServerRunning)
    return;

  if (gOtaEnabled) {
    ElegantOTA.clearAuth();
    return;
  }

  char user[9];
  char pass[17];
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  snprintf(user, sizeof(user), "%08lx",
           static_cast<unsigned long>(r1 & 0xFFFFFFFFu));
  snprintf(pass, sizeof(pass), "%08lx%08lx",
           static_cast<unsigned long>(r1 & 0xFFFFFFFFu),
           static_cast<unsigned long>(r2 & 0xFFFFFFFFu));
  ElegantOTA.setAuth(user, pass);
}

void resetOtaProgressState() {
  gOtaInProgress = false;
  gOtaStartAt = 0;
  gOtaReceivedBytes = false;
  gLastProgressLog = 0;
}

void stopOtaServer() {
  resetOtaProgressState();
}

void startOtaServer() {
  if (gServerRunning || !gOtaEnabled)
    return;

  gOtaServer = wifiManagerHttpServer();
  if (!gOtaServer) {
    logPrintln("[ota] HTTP server unavailable for OTA");
    return;
  }

  ElegantOTA.begin(gOtaServer);
  ElegantOTA.onStart([]() {
    gOtaInProgress = true;
    gOtaStartAt = millis();
    gOtaReceivedBytes = false;
    gLastProgressLog = gOtaStartAt;
    logPrintln("[ota] OTA update started");
    updateToggleCharacteristic(true);
  });
  ElegantOTA.onProgress([](size_t current, size_t final) {
    if (!gOtaReceivedBytes && current > 0) {
      gOtaReceivedBytes = true;
      logPrintln("[ota] OTA upload stream detected");
    }

    unsigned long now = millis();
    if (now - gLastProgressLog >= kProgressLogIntervalMs) {
      gLastProgressLog = now;
      logPrintf("[ota] OTA progress %lu / %lu bytes\n",
                static_cast<unsigned long>(current),
                static_cast<unsigned long>(final));
    }
  });
  ElegantOTA.onEnd([](bool success) {
    resetOtaProgressState();
    gOtaEnableAt = millis();
    if (success) {
      logPrintln("[ota] OTA update finished successfully");
      gOtaEnabled = false;
    } else {
      logPrintln("[ota] OTA update failed");
    }
    updateToggleCharacteristic(true);
  });

  gServerRunning = true;
  applyOtaAuthGuard();
  logPrintf("[ota] ElegantOTA server started on port %u\n",
            static_cast<unsigned>(kOtaHttpPort));
  logOtaEntryPoints();
}

void setOtaEnabled(bool enabled) {
  if (enabled) {
    if (!gOtaEnabled) {
      logPrintln("[ota] OTA updates enabled via BLE");
    }
    gOtaEnabled = true;
    gOtaEnableAt = millis();
    gLastProgressLog = 0;
    updateToggleCharacteristic(true);
    applyOtaAuthGuard();
    return;
  }

  if (!gOtaEnabled)
    return;

  logPrintln("[ota] OTA updates disabled");
  gOtaEnabled = false;
  updateToggleCharacteristic(true);
  applyOtaAuthGuard();
}

void OtaToggleCallbacks::onWrite(NimBLECharacteristic *characteristic) {
  const std::string &value = characteristic->getValue();
  bool enable = !value.empty() && value[0] == '1';
  setOtaEnabled(enable);
}

} // namespace

void initOtaService(NimBLEService *service) {
  if (!service)
    return;

  gToggleChar = service->createCharacteristic(
      OTA_ENABLE_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
          NIMBLE_PROPERTY::NOTIFY);

  if (!gToggleChar) {
    logPrintln("[ota] Failed to create OTA enable characteristic");
    return;
  }

  gToggleChar->setCallbacks(&gToggleCallbacks);
  updateToggleCharacteristic(false);
}

void otaHandleBleDisconnect() {
  if (!gOtaEnabled || gOtaInProgress)
    return;
  logPrintln("[ota] BLE disconnected, closing OTA window");
  setOtaEnabled(false);
}

void otaTick() {
  unsigned long now = millis();

  ElegantOTA.loop();

  // Guard against stalled uploads that never delivered data after start.
  if (gOtaInProgress && !gOtaReceivedBytes && gOtaStartAt &&
      (now - gOtaStartAt) >= kOtaStartTimeoutMs) {
    logPrintln("[ota] OTA stalled waiting for first bytes, aborting");
    if (Update.isRunning()) {
      Update.abort();
    }
    resetOtaProgressState();
    gOtaEnableAt = now;
    updateToggleCharacteristic(true);
  }

  if (gOtaEnabled) {
    ensureApForOta();
    if (!gServerRunning) {
      startOtaServer();
    }
    if (!gOtaInProgress && gOtaEnableAt &&
        (now - gOtaEnableAt) >= kOtaEnableWindowMs) {
      logPrintln("[ota] OTA enable window expired");
      setOtaEnabled(false);
    }
  }

  if (!gOtaEnabled && !gOtaInProgress) {
    stopApIfStartedForOta();
  }
}

bool otaUpdatesEnabled() { return gOtaEnabled; }

bool otaUpdateInProgress() { return gOtaInProgress; }
