#include "gps_ble.h"

NimBLECharacteristic *pCharNavData = nullptr;
NimBLECharacteristic *pCharStatus = nullptr;

NimBLEServer *pServer = nullptr;

static bool bleConnected = false;

static constexpr float kLatLonEps = 1e-5f;
static constexpr float kHeadingEps = 1.0f;
static constexpr float kSpeedEps = 0.2f;
static constexpr float kAltEps = 0.5f;

static float lastLat = 0.0f;
static float lastLon = 0.0f;
static float lastHeading = 0.0f;
static float lastSpeed = 0.0f;
static float lastAlt = 0.0f;
static bool haveLastNav = false;

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server) {
    server->updateConnParams(0, 24, 48, 0, 400);
    bleConnected = true;
    if (pCharStatus) {
      char buf[80];
      int len =
          snprintf(buf, sizeof(buf),
                   "{\"fix\":%u,\"hdop\":%.1f,\"signals\":[],\"ttff\":%d}", 0,
                   100.0f, -1);
      if (len > 0) {
        pCharStatus->setValue((uint8_t *)buf, len);
        pCharStatus->notify();
      }
    }
  }

  void onDisconnect(NimBLEServer *) {
    bleConnected = false;
    NimBLEDevice::startAdvertising();
  }

} serverCallbacks;

class GeneralChrCallbacks : public NimBLECharacteristicCallbacks {

} generalChrCallbacks;

void initBLE() {
  NimBLEDevice::init("ESP32-GPS-BLE");
  NimBLEDevice::setPower(ESP_PWR_LVL_P6);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(&serverCallbacks);

  NimBLEService *pService = pServer->createService(GPS_SERVICE_UUID);

  pCharNavData = pService->createCharacteristic(
      CHAR_NAVDATA_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pCharNavData->setCallbacks(&generalChrCallbacks);

  pCharStatus = pService->createCharacteristic(
      CHAR_STATUS_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pCharStatus->setCallbacks(&generalChrCallbacks);

  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName("GPS-C3");
  pAdvertising->addServiceUUID(GPS_SERVICE_UUID);
  pAdvertising->setScanResponse(false);
  pAdvertising->setMinInterval(0x0800);
  pAdvertising->setMaxInterval(0x1000);
  pAdvertising->start();
}

static inline bool diffExceeds(float a, float b, float eps) {
  float d = a - b;
  if (d < 0)
    d = -d;
  return d > eps;
}

void updateNavData(float lat, float lon, float heading, float speed,
                   float altitude) {
  if (!pCharNavData || !bleConnected)
    return;

  bool needSend = !haveLastNav || diffExceeds(lat, lastLat, kLatLonEps) ||
                  diffExceeds(lon, lastLon, kLatLonEps) ||
                  diffExceeds(heading, lastHeading, kHeadingEps) ||
                  diffExceeds(speed, lastSpeed, kSpeedEps) ||
                  diffExceeds(altitude, lastAlt, kAltEps);

  if (!needSend)
    return;

  lastLat = lat;
  lastLon = lon;
  lastHeading = heading;
  lastSpeed = speed;
  lastAlt = altitude;
  haveLastNav = true;

  char json[112];
  int len = snprintf(
      json, sizeof(json),
      "{\"lt\":%.6f,\"lg\":%.6f,\"hd\":%.1f,\"spd\":%.1f,\"alt\":%.1f}", lat,
      lon, heading, speed, altitude);
  if (len <= 0)
    return;
  pCharNavData->setValue((uint8_t *)json, len);
  pCharNavData->notify();
}

void updateSystemStatus(uint8_t fix, float hdop, const String &signalsArrayJson,
                        int32_t ttffSeconds) {
  if (!pCharStatus || !bleConnected)
    return;

  char json[112];
  int len =
      snprintf(json, sizeof(json),
               "{\"fix\":%u,\"hdop\":%.1f,\"signals\":%s,\"ttff\":%d}",
               (unsigned)fix, hdop, signalsArrayJson.c_str(), (int)ttffSeconds);
  if (len <= 0)
    return;
  pCharStatus->setValue((uint8_t *)json, len);
  pCharStatus->notify();
}
