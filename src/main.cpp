#include "gps_ble.h"
#include "gps_config.h"
#include "led_status.h"
#include <Arduino.h>
#include <iarduino_GPS_NMEA.h>

HardwareSerial gpsSerial(1);

iarduino_GPS_NMEA gps;

extern NimBLEServer *pServer;

static uint32_t lastBLEUpdate = 0;
static uint16_t navUpdateCounter = 0;
static uint32_t bootMillis = 0;
static bool firstFixCaptured = false;
static int32_t ttffSeconds = -1;
uint8_t satsInfo[32][7];

void setup() {
  Serial.begin(115200);
  Serial.println("GPS BLE Booting...");

  initBLE();
  initStatusLED();

  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, HIGH);

  gpsSerial.begin(GPS_BAUD_RATE, SERIAL_8N1, GPS_RX, GPS_TX);

  gps.begin(gpsSerial, true);
  gps.timeOut(1500);

  Serial.println("Boot complete. Ready to work.");
  bootMillis = millis();
}

void loop() {
  gps.read(satsInfo);

  if (millis() - lastBLEUpdate > OUTPUT_INTERVAL_MS) {
    lastBLEUpdate = millis();

    uint8_t fix = (gps.errPos == 0) ? 1 : 0;

    uint8_t systemStatus;
    if (currentStatus == STATUS_BOOTING) {
      systemStatus = STATUS_BOOTING;
    } else if (!fix || gps.satellites[GPS_ACTIVE] < 4) {
      systemStatus = STATUS_NO_FIX;
    } else if (fix && gps.satellites[GPS_ACTIVE] >= 4) {
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
        updateSystemStatus(fix, gps.HDOP, String(signalsJson), ttffSeconds);
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
