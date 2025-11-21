#ifndef GPS_RUNTIME_STATE_H
#define GPS_RUNTIME_STATE_H

#include <stdint.h>

struct SignalStrengthCounters {
  uint8_t weak = 0;
  uint8_t medium = 0;
  uint8_t strong = 0;
};

struct GpsRuntimeState {
  uint8_t satelliteInfo[32][7] = {};
  SignalStrengthCounters signalLevels;
  uint16_t navUpdateCounter = 0;
  uint32_t lastBleUpdate = 0;
  uint32_t bootMillis = 0;
  int32_t ttffSeconds = -1;
  bool firstFixCaptured = false;
  bool passthroughActive = false;
  bool ubxLinkOk = false;
  bool ubxConfigured = false;
};

#endif
