#ifndef GPS_RUNTIME_STATE_H
#define GPS_RUNTIME_STATE_H

#include <stddef.h>
#include <stdint.h>

constexpr size_t kMaxTrackedSatellites = 20;

struct SignalStrengthCounters {
  uint8_t weak = 0;
  uint8_t medium = 0;
  uint8_t strong = 0;
};

struct SatelliteDebugEntry {
  uint8_t id = 0;
  uint8_t snr = 0;
  uint8_t constellation = 0;
  uint8_t active = 0;
  uint8_t elevation = 0;
  uint16_t azimuth = 0;
};

struct GpsRuntimeState {
  uint8_t satelliteInfo[32][7] = {};
  SignalStrengthCounters signalLevels;
  uint8_t activeSignalDb[kMaxTrackedSatellites] = {};
  uint8_t activeSignalCount = 0;
  SatelliteDebugEntry satDebug[kMaxTrackedSatellites] = {};
  uint8_t satDebugCount = 0;
  uint8_t visibleSatellites = 0;
  uint8_t activeSatellites = 0;
  float lastTempC = 0.0f;
  bool tempValid = false;
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
