#include "gps_controller.h"

#include "gps_ble.h"
#include "gps_config.h"
#include "gps_serial_control.h"
#include "led_status.h"
#include "logger.h"
#include "system_mode.h"

#include <Arduino.h>
#include <Preferences.h>
#include <iarduino_GPS_NMEA.h>

namespace {
HardwareSerial gpsSerial(1);
iarduino_GPS_NMEA gpsParser;
constexpr const char *kGpsPrefsNamespace = "gpscfg";
constexpr const char *kGpsBaudKey = "baud";
} // namespace

GpsController &gpsController() {
  static GpsController instance;
  return instance;
}

void GpsController::begin() {
  state = GpsRuntimeState{};
  state.bootMillis = millis();
  gpsSerialBaudValue = loadStoredGpsBaud();
  prevFix = 255;
  prevHdop10 = -1;
  prevStrong = prevMedium = prevWeak = 255;

  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, HIGH);

  configureGpsSerial(true, true);
}

void GpsController::loop() {
  bool passthrough = isSerialPassthroughMode();
  if (passthrough != state.passthroughActive) {
    state.passthroughActive = passthrough;
    if (state.passthroughActive) {
      configureGpsSerial(false, true);
      setStatus(STATUS_READY);
    } else {
      configureGpsSerial(true, true);
      resetNavigationState();
    }
  }

  if (state.passthroughActive) {
    processPassthroughIO();
    delay(1);
    return;
  }

  processNavigationUpdate();
  delay(1);
}

bool GpsController::setBaud(uint32_t baud) {
  if (baud < GPS_BAUD_MIN || baud > GPS_BAUD_MAX)
    return false;

  if (baud == gpsSerialBaudValue)
    return false;

  gpsSerialBaudValue = baud;
  configureGpsSerial(parserEnabled, true);
  logPrintf("[gps] Serial baud updated to %lu\n",
            static_cast<unsigned long>(gpsSerialBaudValue));
  updateGpsBaudCharacteristic(gpsSerialBaudValue);
  persistGpsBaud(gpsSerialBaudValue);
  return true;
}

uint32_t GpsController::baud() const { return gpsSerialBaudValue; }

void GpsController::addNavPublisher(NavDataPublisher *publisher) {
  if (!publisher)
    return;
  for (size_t i = 0; i < navPublisherCount; ++i) {
    if (navPublishers[i] == publisher)
      return;
  }
  if (navPublisherCount < kMaxNavPublishers) {
    navPublishers[navPublisherCount++] = publisher;
  }
}

void GpsController::addStatusPublisher(SystemStatusPublisher *publisher) {
  if (!publisher)
    return;
  for (size_t i = 0; i < statusPublisherCount; ++i) {
    if (statusPublishers[i] == publisher)
      return;
  }
  if (statusPublisherCount < kMaxStatusPublishers) {
    statusPublishers[statusPublisherCount++] = publisher;
  }
}

void GpsController::configureGpsSerial(bool enableParser, bool forceReinit) {
  if (!forceReinit && parserEnabled == enableParser)
    return;

  gpsSerial.flush();
  gpsSerial.end();
  delay(10);

  gpsSerial.begin(gpsSerialBaudValue, SERIAL_8N1, GPS_RX, GPS_TX);

  gpsParser = iarduino_GPS_NMEA();
  if (enableParser) {
    gpsParser.begin(gpsSerial, true);
    gpsParser.timeOut(1500);
  }

  parserEnabled = enableParser;
}

uint32_t GpsController::loadStoredGpsBaud() {
  Preferences prefs;
  uint32_t stored = GPS_BAUD_RATE;
  if (prefs.begin(kGpsPrefsNamespace, true)) {
    uint32_t value = prefs.getUInt(kGpsBaudKey, stored);
    prefs.end();
    if (value >= GPS_BAUD_MIN && value <= GPS_BAUD_MAX) {
      stored = value;
    }
  }
  return stored;
}

void GpsController::persistGpsBaud(uint32_t baud) {
  Preferences prefs;
  if (prefs.begin(kGpsPrefsNamespace, false)) {
    prefs.putUInt(kGpsBaudKey, baud);
    prefs.end();
  }
}

void GpsController::resetNavigationState() {
  state.navUpdateCounter = 0;
  state.firstFixCaptured = false;
  state.ttffSeconds = -1;
  state.signalLevels = {};
  state.lastBleUpdate = millis();
  prevFix = 255;
  prevHdop10 = -1;
  prevStrong = prevMedium = prevWeak = 255;
}

void GpsController::processPassthroughIO() {
  while (gpsSerial.available() > 0) {
    int byteValue = gpsSerial.read();
    if (byteValue >= 0) {
      Serial.write(static_cast<uint8_t>(byteValue));
    }
  }
  while (Serial.available() > 0) {
    int byteValue = Serial.read();
    if (byteValue >= 0) {
      gpsSerial.write(static_cast<uint8_t>(byteValue));
    }
  }
}

void GpsController::processNavigationUpdate() {
  gpsParser.read(state.satelliteInfo);

  unsigned long now = millis();
  if (now - state.lastBleUpdate <= OUTPUT_INTERVAL_MS) {
    return;
  }
  state.lastBleUpdate = now;

  uint8_t fix = (gpsParser.errPos == 0) ? 1 : 0;
  uint8_t activeSatellites = gpsParser.satellites[GPS_ACTIVE];

  uint8_t systemStatus = determineSystemStatus(fix, activeSatellites);
  if (systemStatus != getStatusIndicatorState()) {
    setStatus(systemStatus);
  }

  if (fix && gpsParser.errPos == 0) {
    if (!state.firstFixCaptured) {
      state.firstFixCaptured = true;
      state.ttffSeconds =
          static_cast<int32_t>((now - state.bootMillis) / 1000UL);
    }
    float latDecimal = gpsParser.latitude;
    float lonDecimal = gpsParser.longitude;

    float heading = gpsParser.course;
    if (heading < 0.0f) {
      heading += 360.0f;
    }
    float speedMs = static_cast<float>(gpsParser.speed) * (1000.0f / 3600.0f);

    if (navPublisherCount > 0) {
      NavDataSample navSample;
      navSample.latitude = latDecimal;
      navSample.longitude = lonDecimal;
      navSample.heading = heading;
      navSample.speed = speedMs;
      navSample.altitude = gpsParser.altitude;
      for (size_t i = 0; i < navPublisherCount; ++i) {
        if (navPublishers[i]) {
          navPublishers[i]->publishNavData(navSample);
        }
      }
    }
    state.navUpdateCounter++;
  }

  uint8_t strong = 0, medium = 0, weak = 0;
  for (uint8_t i = 0; i < 20; i++) {
    uint8_t id = state.satelliteInfo[i][0];
    uint8_t snr = state.satelliteInfo[i][1];
    uint8_t active = state.satelliteInfo[i][3];
    if (!id || !active)
      continue;
    if (snr > 30)
      strong++;
    else if (snr >= 20)
      medium++;
    else
      weak++;
  }

  state.signalLevels.weak = weak;
  state.signalLevels.medium = medium;
  state.signalLevels.strong = strong;

  char signalsJson[64];
  int pos = 0;
  signalsJson[pos++] = '[';
  bool first = true;
  auto appendLevel = [&](uint8_t count, char value) {
    for (uint8_t i = 0; i < count; i++) {
      if (!first && pos < static_cast<int>(sizeof(signalsJson)) - 1) {
        signalsJson[pos++] = ',';
      }
      if (pos < static_cast<int>(sizeof(signalsJson)) - 1) {
        signalsJson[pos++] = value;
      }
      first = false;
    }
  };
  appendLevel(weak, '1');
  appendLevel(medium, '2');
  appendLevel(strong, '3');
  if (pos < static_cast<int>(sizeof(signalsJson)) - 1) {
    signalsJson[pos++] = ']';
  }
  signalsJson[(pos < static_cast<int>(sizeof(signalsJson)))
                  ? pos
                  : static_cast<int>(sizeof(signalsJson)) - 1] = '\0';

  if (state.navUpdateCounter >= 20) {
    int hdop10 = static_cast<int>(gpsParser.HDOP * 10.0f + 0.5f);
    bool changed = (prevFix != fix) || (prevHdop10 != hdop10) ||
                   (prevStrong != strong) || (prevMedium != medium) ||
                   (prevWeak != weak);
    if (changed) {
      if (statusPublisherCount > 0) {
        SystemStatusSample statusSample;
        statusSample.fix = fix;
        statusSample.hdop = gpsParser.HDOP;
        statusSample.satellites = activeSatellites;
        statusSample.ttffSeconds = state.ttffSeconds;
        statusSample.signalsJson = String(signalsJson);
        for (size_t i = 0; i < statusPublisherCount; ++i) {
          if (statusPublishers[i]) {
            statusPublishers[i]->publishSystemStatus(statusSample);
          }
        }
      }
      prevFix = fix;
      prevHdop10 = hdop10;
      prevStrong = strong;
      prevMedium = medium;
      prevWeak = weak;
    }
    state.navUpdateCounter = 0;
  }
}

uint8_t GpsController::determineSystemStatus(uint8_t fix,
                                             uint8_t activeSatellites) const {
  if (getStatusIndicatorState() == STATUS_BOOTING) {
    return STATUS_BOOTING;
  }
  if (!fix || activeSatellites < 4) {
    return STATUS_NO_FIX;
  }
  if (fix && activeSatellites >= 4) {
    return STATUS_FIX_SYNC;
  }
  return STATUS_READY;
}

uint32_t getGpsSerialBaud() { return gpsController().baud(); }

bool setGpsSerialBaud(uint32_t baud) {
  return gpsController().setBaud(baud);
}
