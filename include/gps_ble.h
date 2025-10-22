#ifndef GPS_BLE_H
#define GPS_BLE_H

#include <Arduino.h>
#include <NimBLECharacteristic.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>

static const char *GPS_SERVICE_UUID = "14f0514a-e15f-4ad3-89a6-b4cb3ac86abe";
static const char *CHAR_NAVDATA_UUID = "12c64fea-7ed9-40be-9c7e-9912a5050d23";
static const char *CHAR_STATUS_UUID = "3e4f5d6c-7b8a-9d0e-1f2a-3b4c5d6e7f8a";
static const char *CHAR_AP_CONTROL_UUID =
    "a37f8c1b-281d-4e15-8fb2-0b7e6ebd21c0";
static const char *CHAR_MODE_CONTROL_UUID =
    "d047f6b3-5f7c-4e5b-9c21-4c0f2b6a8f10";

extern NimBLECharacteristic *pCharNavData;
extern NimBLECharacteristic *pCharStatus;
extern NimBLECharacteristic *pCharApControl;
extern NimBLECharacteristic *pCharModeControl;

extern NimBLEServer *pServer;

void initBLE();
void updateNavData(float lat, float lon, float heading, float speed,
                   float altitude);
void updateSystemStatus(uint8_t fix, float hdop, uint8_t satellites,
                        const String &signalsArrayJson, int32_t ttffSeconds);
void updateApControlCharacteristic(bool apActive);
void updatePassthroughModeCharacteristic();

#endif
