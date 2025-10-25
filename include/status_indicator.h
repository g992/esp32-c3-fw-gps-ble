#ifndef STATUS_INDICATOR_H
#define STATUS_INDICATOR_H

#include "gps_config.h"
#include <Arduino.h>
#include <stdint.h>

class StatusIndicator {
public:
  void begin();
  void setStatus(uint8_t status);
  void update();
  void onPpsPulse();
  uint8_t status() const;

private:
  void writeLedOn(bool on);

  uint8_t currentStatusValue = STATUS_BOOTING;
  unsigned long lastBlinkTime = 0;
  bool ledState = false;
  volatile bool ppsDetected = false;
  unsigned long bootStartTime = 0;
  unsigned long patternStartTime = 0;
  unsigned long lastLedTick = 0;
  bool lastPinHigh = true;
};

StatusIndicator &statusIndicator();

#endif
