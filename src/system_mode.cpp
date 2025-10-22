#include <Preferences.h>

#include "system_mode.h"
#include "gps_config.h"

#include "wifi_manager.h"

namespace {

OperationMode currentMode = OperationMode::Navigation;
bool logsEnabled = true;

constexpr size_t kMaxHandlers = 4;
ModeChangeHandler handlers[kMaxHandlers];
size_t handlerCount = 0;

Preferences modePrefs;
constexpr const char *kPrefsNamespace = "sysmode";
constexpr const char *kModeKey = "mode";

OperationMode readStoredMode() {
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

void persistMode(OperationMode mode) {
  if (modePrefs.begin(kPrefsNamespace, false)) {
    modePrefs.putUChar(kModeKey, static_cast<uint8_t>(mode));
    modePrefs.end();
  }
}

void resetGpsModem() {
  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, LOW);
  delay(100);
  digitalWrite(GPS_EN, HIGH);
}

void notifyHandlers(OperationMode mode) {
  for (size_t i = 0; i < handlerCount; ++i) {
    if (handlers[i]) {
      handlers[i](mode);
    }
  }
}

void applyMode(OperationMode mode) {
  bool passthrough = (mode == OperationMode::SerialPassthrough);
  logsEnabled = !passthrough;
  wifiManagerSetGnssStreamingEnabled(!passthrough);
}

} // namespace

void initSystemMode() {
  handlerCount = 0;
  logsEnabled = true;
  currentMode = readStoredMode();
  applyMode(currentMode);
}

bool setOperationMode(OperationMode mode) {
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

OperationMode getOperationMode() { return currentMode; }

bool isSerialPassthroughMode() {
  return currentMode == OperationMode::SerialPassthrough;
}

bool systemLogsEnabled() { return logsEnabled; }

void registerModeChangeHandler(ModeChangeHandler handler) {
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
