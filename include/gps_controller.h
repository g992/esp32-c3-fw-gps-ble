#ifndef GPS_CONTROLLER_H
#define GPS_CONTROLLER_H

#include <stdint.h>

#include "data_channel.h"
#include "gps_runtime_state.h"
#include "ubx_command_set.h"

enum class GnssReceiverType : uint8_t { Ublox = 0, GenericNmea = 1 };

struct GpsDebugSnapshot {
  SatelliteDebugEntry satellites[kMaxTrackedSatellites] = {};
  size_t satelliteCount = 0;
  uint8_t signalDb[kMaxTrackedSatellites] = {};
  size_t signalCount = 0;
  uint8_t visibleCount = 0;
  uint8_t activeCount = 0;
  float tempC = 0.0f;
  bool tempValid = false;
  uint32_t uptimeSeconds = 0;
};

class GpsController {
public:
  void begin();
  void loop();
  bool setBaud(uint32_t baud);
  uint32_t baud() const;
  bool setUbxProfile(UbxConfigProfile profile);
  UbxConfigProfile ubxProfile() const;
  bool setUbxSettingsProfile(UbxSettingsProfile profile);
  UbxSettingsProfile ubxSettingsProfile() const;
  bool setReceiverType(GnssReceiverType type);
  GnssReceiverType receiverType() const { return receiverTypeValue; }
  void addNavPublisher(NavDataPublisher *publisher);
  void addStatusPublisher(SystemStatusPublisher *publisher);
  GpsDebugSnapshot debugSnapshot() const;

private:
  void configureGpsSerial(bool enableParser, bool forceReinit);
  uint32_t loadStoredGpsBaud();
  void persistGpsBaud(uint32_t baud);
  GnssReceiverType loadStoredReceiverType();
  void persistReceiverType(GnssReceiverType type);
  void resetNavigationState();
  void processPassthroughIO();
  void processNavigationUpdate();
  uint8_t determineSystemStatus(uint8_t fix, uint8_t activeSatellites) const;
  bool runUbxStartupSequence();
  bool verifyUbxProfile(UbxConfigProfile profile);
  UbxConfigProfile loadStoredUbxProfile();
  void persistUbxProfile(UbxConfigProfile profile);
  UbxSettingsProfile loadStoredUbxSettingsProfile();
  void persistUbxSettingsProfile(UbxSettingsProfile profile);
  void loadStoredCustomCommands();
  bool applyUbxProfile(UbxConfigProfile profile);

  GpsRuntimeState state;
  GnssReceiverType receiverTypeValue = GnssReceiverType::Ublox;
  uint32_t gpsSerialBaudValue = 0;
  UbxConfigProfile currentProfile = UbxConfigProfile::FullSystems;
  UbxSettingsProfile currentSettingsProfile =
      UbxSettingsProfile::DefaultRamBbr;
  bool parserEnabled = false;
  uint8_t prevFix = 255;
  int prevHdop10 = -1;
  uint8_t prevStrong = 255;
  uint8_t prevMedium = 255;
  uint8_t prevWeak = 255;
  static constexpr size_t kMaxNavPublishers = 4;
  static constexpr size_t kMaxStatusPublishers = 4;
  NavDataPublisher *navPublishers[kMaxNavPublishers] = {};
  SystemStatusPublisher *statusPublishers[kMaxStatusPublishers] = {};
  size_t navPublisherCount = 0;
  size_t statusPublisherCount = 0;
};

GpsController &gpsController();

#endif
