#ifndef SYSTEM_MODE_SERVICE_H
#define SYSTEM_MODE_SERVICE_H

#include <Preferences.h>

#include "system_mode.h"

class SystemModeService {
public:
  void begin();
  bool setMode(OperationMode mode);
  OperationMode mode() const;
  bool isPassthrough() const;
  bool logsEnabled() const;
  void subscribe(ModeChangeHandler handler);

private:
  OperationMode readStoredMode();
  void persistMode(OperationMode mode);
  void notifyHandlers(OperationMode mode);
  void applyMode(OperationMode mode);
  void resetGpsModem();

  OperationMode currentMode = OperationMode::Navigation;
  bool logsEnabledFlag = true;

  static constexpr size_t kMaxHandlers = 4;
  ModeChangeHandler handlers[kMaxHandlers] = {};
  size_t handlerCount = 0;

  Preferences modePrefs;
  static constexpr const char *kPrefsNamespace = "sysmode";
  static constexpr const char *kModeKey = "mode";
};

#endif
