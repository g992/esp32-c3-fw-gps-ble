#include "gps_ble.h"
#include "gps_serial_control.h"
#include "system_mode.h"
#include "wifi_manager.h"

NimBLECharacteristic *pCharNavData = nullptr;
NimBLECharacteristic *pCharStatus = nullptr;
NimBLECharacteristic *pCharApControl = nullptr;
NimBLECharacteristic *pCharModeControl = nullptr;
NimBLECharacteristic *pCharGpsBaud = nullptr;

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

static uint8_t apStateValue = '0';
static uint8_t modeStateValue = '0';

static void setGpsBaudCharacteristicValue(uint32_t baud);

static void refreshApControlCharacteristic() {
  if (!pCharApControl)
    return;

  bool apActive = wifiManagerIsApActive();
  uint8_t desired = apActive ? '1' : '0';
  if (apStateValue != desired) {
    apStateValue = desired;
  }
  pCharApControl->setValue(&apStateValue, 1);
}

static void refreshModeCharacteristic() {
  if (!pCharModeControl)
    return;

  bool passthrough = isSerialPassthroughMode();
  uint8_t desired = passthrough ? '1' : '0';
  if (modeStateValue != desired) {
    modeStateValue = desired;
  }
  pCharModeControl->setValue(&modeStateValue, 1);
}

static void onModeChanged(OperationMode) { refreshModeCharacteristic(); }

static void refreshGpsBaudCharacteristic() {
  setGpsBaudCharacteristicValue(getGpsSerialBaud());
}

static void setGpsBaudCharacteristicValue(uint32_t baud) {
  if (!pCharGpsBaud)
    return;

  char buffer[12];
  int len = snprintf(buffer, sizeof(buffer), "%lu",
                     static_cast<unsigned long>(baud));
  if (len <= 0)
    return;

  pCharGpsBaud->setValue(reinterpret_cast<uint8_t *>(buffer), len);
}

static bool isWhitespace(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static bool parseGpsBaudValue(const std::string &value, uint32_t &baudOut) {
  if (value.empty())
    return false;

  size_t start = 0;
  size_t end = value.size();

  while (start < end && isWhitespace(value[start]))
    ++start;
  while (end > start && isWhitespace(value[end - 1]))
    --end;

  if (start == end)
    return false;

  uint64_t parsed = 0;
  for (size_t i = start; i < end; ++i) {
    char c = value[i];
    if (c < '0' || c > '9')
      return false;
    parsed = parsed * 10u + static_cast<uint32_t>(c - '0');
    if (parsed > GPS_BAUD_MAX)
      return false;
  }

  if (parsed < GPS_BAUD_MIN)
    return false;

  baudOut = static_cast<uint32_t>(parsed);
  return true;
}

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
    refreshApControlCharacteristic();
    refreshModeCharacteristic();
    refreshGpsBaudCharacteristic();
  }

  void onDisconnect(NimBLEServer *) {
    bleConnected = false;
    NimBLEDevice::startAdvertising();
  }

} serverCallbacks;

class GeneralChrCallbacks : public NimBLECharacteristicCallbacks {

} generalChrCallbacks;

class ApControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) {
    const std::string &value = characteristic->getValue();
    bool enable = !value.empty() && (value[0] == '1');
    wifiManagerHandleBleRequest(enable);
    refreshApControlCharacteristic();
  }

  void onRead(NimBLECharacteristic *) {
    refreshApControlCharacteristic();
  }
} apControlCallbacks;

class ModeControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) {
    const std::string &value = characteristic->getValue();
    bool enablePassthrough = !value.empty() && (value[0] == '1');
    OperationMode desired = enablePassthrough
                                ? OperationMode::SerialPassthrough
                                : OperationMode::Navigation;
    setOperationMode(desired);
    refreshModeCharacteristic();
  }

  void onRead(NimBLECharacteristic *) { refreshModeCharacteristic(); }
} modeControlCallbacks;

class GpsBaudCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) {
    const std::string &value = characteristic->getValue();
    uint32_t baud = 0;
    if (parseGpsBaudValue(value, baud)) {
      setGpsSerialBaud(baud);
    }
    refreshGpsBaudCharacteristic();
  }

  void onRead(NimBLECharacteristic *) { refreshGpsBaudCharacteristic(); }
} gpsBaudCallbacks;

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

  pCharApControl = pService->createCharacteristic(
      CHAR_AP_CONTROL_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pCharApControl->setCallbacks(&apControlCallbacks);
  refreshApControlCharacteristic();

  pCharModeControl = pService->createCharacteristic(
      CHAR_MODE_CONTROL_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pCharModeControl->setCallbacks(&modeControlCallbacks);
  registerModeChangeHandler(onModeChanged);
  refreshModeCharacteristic();

  pCharGpsBaud = pService->createCharacteristic(
      CHAR_GPS_BAUD_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pCharGpsBaud->setCallbacks(&gpsBaudCallbacks);
  refreshGpsBaudCharacteristic();

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

  wifiManagerUpdateNavSnapshot(lat, lon, heading, speed, altitude);

  if (!pCharNavData || !bleConnected)
    return;

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

void updateSystemStatus(uint8_t fix, float hdop, uint8_t satellites,
                        const String &signalsArrayJson, int32_t ttffSeconds) {
  wifiManagerUpdateStatusSnapshot(fix, hdop, signalsArrayJson, ttffSeconds,
                                  satellites);

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

void updateApControlCharacteristic(bool apActive) {
  uint8_t desired = apActive ? '1' : '0';
  apStateValue = desired;
  if (pCharApControl) {
    pCharApControl->setValue(&apStateValue, 1);
  }
}

void updatePassthroughModeCharacteristic() { refreshModeCharacteristic(); }

void updateGpsBaudCharacteristic(uint32_t baud) {
  setGpsBaudCharacteristicValue(baud);
}
