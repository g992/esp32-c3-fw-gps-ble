#include "gps_ble.h"
#include "data_channel.h"
#include "firmware_app.h"
#include "gps_serial_control.h"
#include "logger.h"
#include "ota_service.h"
#include "system_mode.h"
#include "wifi_manager.h"
#include <Arduino.h>

NimBLECharacteristic *pCharNavData = nullptr;
NimBLECharacteristic *pCharStatus = nullptr;
NimBLECharacteristic *pCharApControl = nullptr;
NimBLECharacteristic *pCharModeControl = nullptr;
NimBLECharacteristic *pCharGpsBaud = nullptr;
NimBLECharacteristic *pCharUbxProfile = nullptr;
NimBLECharacteristic *pCharKeepAlive = nullptr;

NimBLEServer *pServer = nullptr;

static bool bleConnected = false;
static uint16_t currentConnHandle = 0xFFFF;
static unsigned long lastKeepAliveMillis = 0;
static bool keepAliveTimeoutPaused = false;
static constexpr unsigned long kKeepAliveTimeoutMs = 10000;

static constexpr float kLatLonEps = 1e-5f;
static constexpr float kHeadingEps = 1.0f;
static constexpr float kSpeedEps = 0.2f;
static constexpr float kAltEps = 0.5f;

static uint8_t apStateValue = '0';
static uint8_t modeStateValue = '0';
static uint8_t ubxProfileStateValue = '0';

class BleDataPublisher : public NavDataPublisher, public SystemStatusPublisher {
public:
  void publishNavData(const NavDataSample &sample) override;
  void publishSystemStatus(const SystemStatusSample &sample) override;

private:
  float lastLat = 0.0f;
  float lastLon = 0.0f;
  float lastHeading = 0.0f;
  float lastSpeed = 0.0f;
  float lastAlt = 0.0f;
  bool haveLastNav = false;
};

static BleDataPublisher gBlePublisher;

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

static void refreshUbxProfileCharacteristic() {
  if (!pCharUbxProfile)
    return;
  UbxConfigProfile profile = getGpsUbxProfile();
  ubxProfileStateValue = static_cast<uint8_t>(ubxProfileToChar(profile));
  pCharUbxProfile->setValue(&ubxProfileStateValue, 1);
}

static void setGpsBaudCharacteristicValue(uint32_t baud) {
  if (!pCharGpsBaud)
    return;

  char buffer[12];
  int len =
      snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(baud));
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
  void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override {
    logPrintln("[ble] Client connected");
    uint16_t handle = desc ? desc->conn_handle : 0;
    if (handle != 0) {
      server->updateConnParams(handle, 24, 48, 0, 400);
    }
    if (handle == 0) {
      auto peers = server->getPeerDevices();
      if (!peers.empty()) {
        handle = peers.front();
      }
    }
    bleConnected = true;
    currentConnHandle = (handle != 0) ? handle : 0xFFFF;
    lastKeepAliveMillis = millis();
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
    refreshUbxProfileCharacteristic();
  }

  void onConnect(NimBLEServer *server) override { onConnect(server, nullptr); }

  void onDisconnect(NimBLEServer *pServer, ble_gap_conn_desc *desc) override {
    Serial.println("[ble] Client disconnected");
    logPrintln("[ble] Client disconnected");
    bleConnected = false;
    currentConnHandle = 0xFFFF;
    lastKeepAliveMillis = 0;
    keepAliveTimeoutPaused = false;
    otaHandleBleDisconnect();
    if (pServer) {
      pServer->startAdvertising();
    }
  }

  void onDisconnect(NimBLEServer *pServer) override {
    onDisconnect(pServer, nullptr);
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

  void onRead(NimBLECharacteristic *) { refreshApControlCharacteristic(); }
} apControlCallbacks;

class ModeControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) {
    const std::string &value = characteristic->getValue();
    bool enablePassthrough = !value.empty() && (value[0] == '1');
    OperationMode desired = enablePassthrough ? OperationMode::SerialPassthrough
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

class UbxProfileCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) {
    const std::string &value = characteristic->getValue();
    if (value.empty())
      return;
    UbxConfigProfile profile;
    if (!ubxProfileFromChar(value[0], profile))
      return;
    if (!setGpsUbxProfile(profile)) {
      logPrintln("[ble] Failed to apply UBX profile");
    }
    refreshUbxProfileCharacteristic();
  }

  void onRead(NimBLECharacteristic *) { refreshUbxProfileCharacteristic(); }
} ubxProfileCallbacks;

class KeepAliveCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) {
    (void)characteristic;
    lastKeepAliveMillis = millis();
  }
} keepAliveCallbacks;

int bleGapEventHandler(ble_gap_event *event, void *arg) {
  (void)arg;
  if (!event)
    return 0;

  switch (event->type) {
  case BLE_GAP_EVENT_DISCONNECT:
    lastKeepAliveMillis = 0;
    break;

  default:
    lastKeepAliveMillis = millis();
    break;
  }
  return 0;
}

void initBLE() {
  NimBLEDevice::init("ESP32-GPS-BLE");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(false, false, false);
  NimBLEDevice::setCustomGapHandler(bleGapEventHandler);
  NimBLEDevice::setCustomGapHandler(bleGapEventHandler);

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

  pCharUbxProfile = pService->createCharacteristic(
      CHAR_UBX_PROFILE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
  pCharUbxProfile->setCallbacks(&ubxProfileCallbacks);
  refreshUbxProfileCharacteristic();

  pCharKeepAlive = pService->createCharacteristic(CHAR_KEEPALIVE_UUID,
                                                  NIMBLE_PROPERTY::WRITE);
  pCharKeepAlive->setCallbacks(&keepAliveCallbacks);

  pService->start();
  initOtaService(pServer);

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->setName("GPS-C3");
  pAdvertising->addServiceUUID(GPS_SERVICE_UUID);
  pAdvertising->addServiceUUID(OTA_SERVICE_UUID);
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

void BleDataPublisher::publishNavData(const NavDataSample &sample) {
  bool needSend = !haveLastNav ||
                  diffExceeds(sample.latitude, lastLat, kLatLonEps) ||
                  diffExceeds(sample.longitude, lastLon, kLatLonEps) ||
                  diffExceeds(sample.heading, lastHeading, kHeadingEps) ||
                  diffExceeds(sample.speed, lastSpeed, kSpeedEps) ||
                  diffExceeds(sample.altitude, lastAlt, kAltEps);

  if (!needSend)
    return;

  lastLat = sample.latitude;
  lastLon = sample.longitude;
  lastHeading = sample.heading;
  lastSpeed = sample.speed;
  lastAlt = sample.altitude;
  haveLastNav = true;

  if (!pCharNavData || !bleConnected)
    return;

  char json[112];
  int len = snprintf(
      json, sizeof(json),
      "{\"lt\":%.6f,\"lg\":%.6f,\"hd\":%.1f,\"spd\":%.1f,\"alt\":%.1f}",
      sample.latitude, sample.longitude, sample.heading, sample.speed,
      sample.altitude);
  if (len <= 0)
    return;
  pCharNavData->setValue((uint8_t *)json, len);
  pCharNavData->notify();
}

void BleDataPublisher::publishSystemStatus(const SystemStatusSample &sample) {
  if (!pCharStatus || !bleConnected)
    return;

  char json[112];
  int len = snprintf(json, sizeof(json),
                     "{\"fix\":%u,\"hdop\":%.1f,\"signals\":%s,\"ttff\":%d}",
                     static_cast<unsigned>(sample.fix), sample.hdop,
                     sample.signalsJson.c_str(),
                     static_cast<int>(sample.ttffSeconds));
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

void updateUbxProfileCharacteristic(UbxConfigProfile profile) {
  ubxProfileStateValue = static_cast<uint8_t>(ubxProfileToChar(profile));
  if (pCharUbxProfile) {
    pCharUbxProfile->setValue(&ubxProfileStateValue, 1);
  }
}

NavDataPublisher *bleNavPublisher() { return &gBlePublisher; }

SystemStatusPublisher *bleStatusPublisher() { return &gBlePublisher; }

bool bleHasActiveConnection() {
  if (pServer && pServer->getConnectedCount() > 0)
    return true;
  return bleConnected;
}

void bleTick() {
  if (!bleConnected)
    return;
  if (currentConnHandle == 0xFFFF)
    return;

  bool otaActive = otaSessionActive();
  if (otaActive) {
    keepAliveTimeoutPaused = true;
    return;
  }
  if (keepAliveTimeoutPaused) {
    keepAliveTimeoutPaused = false;
    lastKeepAliveMillis = millis();
  }

  unsigned long now = millis();
  if (lastKeepAliveMillis != 0 &&
      now - lastKeepAliveMillis <= kKeepAliveTimeoutMs)
    return;

  Serial.println("[ble] Keepalive timeout, disconnecting client");
  logPrintln("[ble] Keepalive timeout, disconnecting client");
  if (pServer) {
    pServer->disconnect(currentConnHandle);
    pServer->startAdvertising();
  }
  bleConnected = false;
  currentConnHandle = 0xFFFF;
  lastKeepAliveMillis = 0;
}
