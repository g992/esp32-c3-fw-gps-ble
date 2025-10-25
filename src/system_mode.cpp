#include "system_mode_service.h"
#include "gps_config.h"
#include "system_mode.h"
#include "wifi_manager.h"

#include <Arduino.h>

SystemModeService &systemModeService() {
  static SystemModeService instance;
  return instance;
}

void SystemModeService::begin() {
  handlerCount = 0;
  logsEnabledFlag = true;
  currentMode = readStoredMode();
  applyMode(currentMode);
}

bool SystemModeService::setMode(OperationMode mode) {
  if (mode == currentMode) {
    return false;
  }
  currentMode = mode;
  persistMode(mode);
  applyMode(mode);
  resetGpsModem();
  notifyHandlers(mode);
  return true;
}

OperationMode SystemModeService::mode() const { return currentMode; }

bool SystemModeService::isPassthrough() const {
  return currentMode == OperationMode::SerialPassthrough;
}

bool SystemModeService::logsEnabled() const { return logsEnabledFlag; }

void SystemModeService::subscribe(ModeChangeHandler handler) {
  if (!handler) {
    return;
  }
  for (size_t i = 0; i < handlerCount; ++i) {
    if (handlers[i] == handler) {
      return;
    }
  }
  if (handlerCount < kMaxHandlers) {
    handlers[handlerCount++] = handler;
  }
}

OperationMode SystemModeService::readStoredMode() {
  OperationMode stored = OperationMode::Navigation;
  if (modePrefs.begin(kPrefsNamespace, true)) {
    uint8_t value =
        modePrefs.getUChar(kModeKey, static_cast<uint8_t>(stored));
    modePrefs.end();
    if (value <= static_cast<uint8_t>(OperationMode::SerialPassthrough)) {
      stored = static_cast<OperationMode>(value);
    }
  }
  return stored;
}

void SystemModeService::persistMode(OperationMode mode) {
  if (modePrefs.begin(kPrefsNamespace, false)) {
    modePrefs.putUChar(kModeKey, static_cast<uint8_t>(mode));
    modePrefs.end();
  }
}

void SystemModeService::resetGpsModem() {
  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, LOW);
  delay(100);
  digitalWrite(GPS_EN, HIGH);
}

void SystemModeService::notifyHandlers(OperationMode mode) {
  for (size_t i = 0; i < handlerCount; ++i) {
    if (handlers[i]) {
      handlers[i](mode);
    }
  }
}

void SystemModeService::applyMode(OperationMode mode) {
  bool passthrough = (mode == OperationMode::SerialPassthrough);
  logsEnabledFlag = !passthrough;
  wifiManagerSetGnssStreamingEnabled(!passthrough);
}

void initSystemMode() { systemModeService().begin(); }

bool setOperationMode(OperationMode mode) {
  return systemModeService().setMode(mode);
}

OperationMode getOperationMode() { return systemModeService().mode(); }

bool isSerialPassthroughMode() {
  return systemModeService().isPassthrough();
}

bool systemLogsEnabled() { return systemModeService().logsEnabled(); }

void registerModeChangeHandler(ModeChangeHandler handler) {
  systemModeService().subscribe(handler);
}
