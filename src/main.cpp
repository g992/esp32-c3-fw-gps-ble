#include "gps_ble.h"
#include "gps_config.h"
#include "gps_serial_control.h"
#include "led_status.h"
#include "logger.h"
#include "system_mode.h"
#include "wifi_manager.h"
#include <Arduino.h>
#include <Preferences.h>
#include <iarduino_GPS_NMEA.h>

HardwareSerial gpsSerial(1);
static uint32_t gpsSerialBaud = GPS_BAUD_RATE;
static constexpr const char *kGpsPrefsNamespace = "gpscfg";
static constexpr const char *kGpsBaudKey = "baud";

iarduino_GPS_NMEA gps;
static bool gpsParserEnabled = false;

extern NimBLEServer *pServer;

static uint32_t lastBLEUpdate = 0;
static uint16_t navUpdateCounter = 0;
static uint32_t bootMillis = 0;
static bool firstFixCaptured = false;
static int32_t ttffSeconds = -1;
uint8_t satsInfo[32][7];
static bool passthroughActive = false;

static uint32_t loadStoredGpsBaud();
static void persistGpsBaud(uint32_t baud);
static void configureGpsSerial(bool enableParser, bool forceReinit);

uint32_t getGpsSerialBaud() { return gpsSerialBaud; }

bool setGpsSerialBaud(uint32_t baud) {
  if (baud < GPS_BAUD_MIN || baud > GPS_BAUD_MAX)
    return false;

  if (baud == gpsSerialBaud)
    return false;

  gpsSerialBaud = baud;

  configureGpsSerial(gpsParserEnabled, true);
  logPrintf("[gps] Serial baud updated to %lu\n",
            static_cast<unsigned long>(gpsSerialBaud));
  updateGpsBaudCharacteristic(gpsSerialBaud);
  persistGpsBaud(gpsSerialBaud);
  return true;
}

static uint32_t loadStoredGpsBaud() {
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

static void persistGpsBaud(uint32_t baud) {
  Preferences prefs;
  if (prefs.begin(kGpsPrefsNamespace, false)) {
    prefs.putUInt(kGpsBaudKey, baud);
    prefs.end();
  }
}

static void configureGpsSerial(bool enableParser, bool forceReinit) {
  if (!forceReinit && gpsParserEnabled == enableParser)
    return;

  gpsSerial.flush();
  gpsSerial.end();
  delay(10);

  gpsSerial.begin(gpsSerialBaud, SERIAL_8N1, GPS_RX, GPS_TX);

  gps = iarduino_GPS_NMEA();
  if (enableParser) {
    gps.begin(gpsSerial, true);
    gps.timeOut(1500);
  }

  gpsParserEnabled = enableParser;
}

static void onWifiApStateChanged(bool active) {
  updateApControlCharacteristic(active);
}

void setup() {
  Serial.begin(115200);
  initSystemMode();
  logPrintln("[sys] Booting firmware...");

  gpsSerialBaud = loadStoredGpsBaud();
  configureGpsSerial(true, true);

  initBLE();
  initStatusLED();
  initWifiManager(onWifiApStateChanged);
  updateApControlCharacteristic(wifiManagerIsApActive());

  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, HIGH);

  logPrintln("[sys] Boot complete.");
  bootMillis = millis();
}

void loop() {
  updateWifiManager();

  bool passthrough = isSerialPassthroughMode();
  if (passthrough != passthroughActive) {
    passthroughActive = passthrough;
    if (passthroughActive) {
      configureGpsSerial(false, true);
      setStatus(STATUS_READY);
    } else {
      configureGpsSerial(true, true);
      navUpdateCounter = 0;
      firstFixCaptured = false;
      ttffSeconds = -1;
      lastBLEUpdate = millis();
    }
  }

  if (passthrough) {
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
    updateStatusLED();
    delay(1);
    return;
  }

  gps.read(satsInfo);

  if (millis() - lastBLEUpdate > OUTPUT_INTERVAL_MS) {
    lastBLEUpdate = millis();

    uint8_t fix = (gps.errPos == 0) ? 1 : 0;
    uint8_t activeSatellites = gps.satellites[GPS_ACTIVE];

    uint8_t systemStatus;
    if (currentStatus == STATUS_BOOTING) {
      systemStatus = STATUS_BOOTING;
    } else if (!fix || activeSatellites < 4) {
      systemStatus = STATUS_NO_FIX;
    } else if (fix && activeSatellites >= 4) {
      systemStatus = STATUS_FIX_SYNC;
    } else {
      systemStatus = STATUS_READY;
    }

    if (systemStatus != currentStatus) {
      setStatus(systemStatus);
    }

    if (fix && gps.errPos == 0) {
      if (!firstFixCaptured) {
        firstFixCaptured = true;
        ttffSeconds = (int32_t)((millis() - bootMillis) / 1000UL);
      }
      float latDecimal = gps.latitude;
      float lonDecimal = gps.longitude;

      float heading = gps.course;
      if (heading < 0.0f) {
        heading += 360.0f;
      }
      float speedMs = static_cast<float>(gps.speed) * (1000.0f / 3600.0f);

      updateNavData(latDecimal, lonDecimal, heading, speedMs, gps.altitude);
      navUpdateCounter++;
    }

    uint8_t strong = 0, medium = 0, weak = 0;
    for (uint8_t i = 0; i < 20; i++) {
      uint8_t id = satsInfo[i][0];
      uint8_t snr = satsInfo[i][1];
      uint8_t active = satsInfo[i][3];
      if (!id || !active)
        continue;
      if (snr > 30)
        strong++;
      else if (snr >= 20)
        medium++;
      else
        weak++;
    }

    char signalsJson[64];
    int pos = 0;
    signalsJson[pos++] = '[';
    bool first = true;
    auto appendLevel = [&](uint8_t count, char value) {
      for (uint8_t i = 0; i < count; i++) {
        if (!first && pos < (int)sizeof(signalsJson) - 1) {
          signalsJson[pos++] = ',';
        }
        if (pos < (int)sizeof(signalsJson) - 1) {
          signalsJson[pos++] = value;
        }
        first = false;
      }
    };
    appendLevel(weak, '1');
    appendLevel(medium, '2');
    appendLevel(strong, '3');
    if (pos < (int)sizeof(signalsJson) - 1) {
      signalsJson[pos++] = ']';
    }
    signalsJson[(pos < (int)sizeof(signalsJson))
                    ? pos
                    : (int)sizeof(signalsJson) - 1] = '\0';

    if (navUpdateCounter >= 20) {
      static uint8_t prevFix = 255;
      static int prevHdop10 = -1;
      static uint8_t prevStrong = 255, prevMedium = 255, prevWeak = 255;
      int hdop10 = (int)(gps.HDOP * 10.0f + 0.5f);
      bool changed = (prevFix != fix) || (prevHdop10 != hdop10) ||
                     (prevStrong != strong) || (prevMedium != medium) ||
                     (prevWeak != weak);
      if (changed) {
        updateSystemStatus(fix, gps.HDOP, activeSatellites,
                           String(signalsJson), ttffSeconds);
        prevFix = fix;
        prevHdop10 = hdop10;
        prevStrong = strong;
        prevMedium = medium;
        prevWeak = weak;
      }
      navUpdateCounter = 0;
    }
  }

  updateStatusLED();

  delay(1);
}
