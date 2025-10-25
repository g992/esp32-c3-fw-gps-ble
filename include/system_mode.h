#ifndef SYSTEM_MODE_H
#define SYSTEM_MODE_H

#include <Arduino.h>

enum class OperationMode : uint8_t {
  Navigation = 0,
  SerialPassthrough = 1,
};

using ModeChangeHandler = void (*)(OperationMode mode);

void initSystemMode();
bool setOperationMode(OperationMode mode);
OperationMode getOperationMode();
bool isSerialPassthroughMode();
bool systemLogsEnabled();
void registerModeChangeHandler(ModeChangeHandler handler);

class SystemModeService;
SystemModeService &systemModeService();

#endif
