#include "wifi_manager.h"

#include "gps_config.h"
#include "logger.h"
#include "ota_service.h"
#include "web_index.h"
#include "web_portal.h"
#include "build_version.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <cstring>
#include <pb_encode.h>

#include "location.pb.h"

namespace {

struct WifiCredentials {
  String ssid;
  String password;
  bool valid = false;
};

Preferences prefs;
WifiCredentials storedCreds;

ApStateCallback apCallback = nullptr;

bool apRequested = false;
bool apActive = false;
bool dnsRunning = false;
bool webServerStarted = false;
bool stationConnecting = false;
bool stationConnectPending = false;
bool mdnsStarted = false;
bool gnssStreamingEnabled = true;

enum class ApRequestSource { None, Button, Ble };

ApRequestSource pendingApSource = ApRequestSource::None;
ApRequestSource activeApSource = ApRequestSource::None;

DNSServer dnsServer;
WebServer webServer(80);

IPAddress apIp(192, 168, 4, 1);

unsigned long buttonPressStarted = 0;
bool buttonTriggered = false;

unsigned long lastReconnectAttempt = 0;
constexpr unsigned long kReconnectIntervalMs = 15000;

unsigned long scheduleApStopAt = 0;

String apSsid;

wl_status_t lastWifiStatus = WL_NO_SHIELD;

void ensureApSsid() {
  if (!apSsid.isEmpty()) {
    return;
  }
  uint32_t chipId = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFF);
  char buf[32];
  snprintf(buf, sizeof(buf), "GPS-C3-%06X", chipId);
  apSsid = buf;
}

struct NavSnapshot {
  bool valid = false;
  float latitude = 0.0f;
  float longitude = 0.0f;
  float heading = 0.0f;
  float speed = 0.0f;
  float altitude = 0.0f;
  unsigned long updatedAt = 0;
  int64_t timestampMs = 0;
};

struct StatusSnapshot {
  bool valid = false;
  uint8_t fix = 0;
  float hdop = 0.0f;
  String signals;
  int32_t ttffSeconds = -1;
  uint8_t satellites = 0;
  unsigned long updatedAt = 0;
};

NavSnapshot navSnapshot;
StatusSnapshot statusSnapshot;

class WifiManagerPublisher : public NavDataPublisher,
                             public SystemStatusPublisher {
public:
  void publishNavData(const NavDataSample &sample) override;
  void publishSystemStatus(const SystemStatusSample &sample) override;
};

static WifiManagerPublisher gWifiPublisher;

constexpr uint16_t kGnssServerPort = 8887;
constexpr size_t kMaxTcpClients = 4;
constexpr unsigned long kHeartbeatTimeoutMs = 4000;
constexpr unsigned long kBroadcastIntervalMs = 1000;
constexpr uint8_t kHeartbeatByte = 0x01;

WiFiServer gnssTcpServer(kGnssServerPort);

struct TcpClientSlot {
  WiFiClient client;
  bool active = false;
  unsigned long lastHeartbeat = 0;
  unsigned long lastSend = 0;
};

TcpClientSlot tcpClients[kMaxTcpClients];

uint8_t pbPayloadBuffer[256];
size_t pbPayloadSize = 0;
bool pbPayloadValid = false;
bool pbPayloadDirty = true;
bool pendingBroadcast = true;
unsigned long pbPayloadBuiltAt = 0;

const char kWaitingStatus[] = "Ожидается фиксация...";
const char kReadyStatus[] = "Готово";
const char kProviderGps[] = "gps";

bool encodeStringCallback(pb_ostream_t *stream, const pb_field_t *field,
                          void *const *arg) {
  const char *str = reinterpret_cast<const char *>(*arg);
  size_t len = strlen(str);
  if (!pb_encode_tag_for_field(stream, field))
    return false;
  return pb_encode_string(stream, reinterpret_cast<const uint8_t *>(str), len);
}

void markPayloadDirty() {
  pbPayloadDirty = true;
  pendingBroadcast = true;
}

void disconnectClient(TcpClientSlot &slot, const char *reason) {
  if (!slot.active)
    return;
  if (slot.client) {
    if (reason) {
      logPrintf("[wifi] TCP client disconnected (%s)\n", reason);
    }
    slot.client.stop();
  }
  slot.active = false;
  slot.lastHeartbeat = 0;
  slot.lastSend = 0;
}

bool buildServerPayload(unsigned long now) {
  gnss_ServerResponse response = gnss_ServerResponse_init_zero;

  bool haveFix =
      statusSnapshot.valid && statusSnapshot.fix && navSnapshot.valid;
  if (haveFix) {
    response.which_response = gnss_ServerResponse_location_update_tag;
    gnss_LocationUpdate &loc = response.response.location_update;
    loc.timestamp = navSnapshot.timestampMs != 0 ? navSnapshot.timestampMs
                                                 : static_cast<int64_t>(now);
    loc.latitude = navSnapshot.latitude;
    loc.longitude = navSnapshot.longitude;
    loc.altitude = navSnapshot.altitude;
    loc.speed = navSnapshot.speed;
    loc.bearing = navSnapshot.heading;
    loc.satellites = statusSnapshot.satellites;
    float ageSeconds = (now >= navSnapshot.updatedAt)
                           ? ((now - navSnapshot.updatedAt) / 1000.0f)
                           : 0.0f;
    loc.location_age = ageSeconds;
    if (statusSnapshot.hdop > 0.0f) {
      float accuracyMeters = statusSnapshot.hdop * 5.0f;
      if (accuracyMeters < 3.0f) {
        accuracyMeters = 3.0f;
      }
      loc.accuracy = accuracyMeters;
    } else {
      loc.accuracy = 0.0f;
    }
    loc.provider.funcs.encode = encodeStringCallback;
    loc.provider.arg = const_cast<char *>(kProviderGps);
  } else {
    response.which_response = gnss_ServerResponse_status_tag;
    const char *statusPtr = kWaitingStatus;
    if (statusSnapshot.valid && statusSnapshot.fix && !navSnapshot.valid) {
      statusPtr = kWaitingStatus;
    } else if (statusSnapshot.valid && statusSnapshot.fix) {
      statusPtr = kReadyStatus;
    }
    response.response.status.funcs.encode = encodeStringCallback;
    response.response.status.arg = const_cast<char *>(statusPtr);
  }

  pb_ostream_t stream =
      pb_ostream_from_buffer(pbPayloadBuffer, sizeof(pbPayloadBuffer));
  if (!pb_encode(&stream, gnss_ServerResponse_fields, &response)) {
    logPrintf("[wifi] Failed to encode ServerResponse: %s\n",
              PB_GET_ERROR(&stream));
    return false;
  }

  pbPayloadSize = stream.bytes_written;
  pbPayloadValid = true;
  pbPayloadDirty = false;
  pbPayloadBuiltAt = now;
  return true;
}

bool ensurePayload(unsigned long now) {
  if (!pbPayloadValid || pbPayloadDirty ||
      (now - pbPayloadBuiltAt) >= kBroadcastIntervalMs) {
    if (!buildServerPayload(now)) {
      return false;
    }
  }
  return pbPayloadValid;
}

bool sendPayloadToClient(TcpClientSlot &slot, unsigned long now) {
  if (!ensurePayload(now)) {
    return false;
  }
  if (!slot.client.connected()) {
    return false;
  }

  uint32_t length = static_cast<uint32_t>(pbPayloadSize);
  uint8_t header[4];
  header[0] = static_cast<uint8_t>((length >> 24) & 0xFF);
  header[1] = static_cast<uint8_t>((length >> 16) & 0xFF);
  header[2] = static_cast<uint8_t>((length >> 8) & 0xFF);
  header[3] = static_cast<uint8_t>(length & 0xFF);

  size_t written = slot.client.write(header, sizeof(header));
  if (written != sizeof(header)) {
    logPrintln("[wifi] Failed to write payload header");
    return false;
  }
  written = slot.client.write(pbPayloadBuffer, pbPayloadSize);
  if (written != pbPayloadSize) {
    logPrintln("[wifi] Failed to write payload body");
    return false;
  }
  slot.client.flush();
  slot.lastSend = now;
  return true;
}

void handleNewTcpClients(unsigned long now) {
  while (true) {
    WiFiClient incoming = gnssTcpServer.available();
    if (!incoming) {
      break;
    }

    int freeIndex = -1;
    for (size_t i = 0; i < kMaxTcpClients; ++i) {
      if (!tcpClients[i].active) {
        freeIndex = static_cast<int>(i);
        break;
      }
    }

    if (freeIndex < 0) {
      logPrintln("[wifi] Rejecting TCP client: no free slots");
      incoming.stop();
      continue;
    }

    tcpClients[freeIndex].client.stop();
    tcpClients[freeIndex].client = incoming;
    tcpClients[freeIndex].client.setNoDelay(true);
    tcpClients[freeIndex].active = true;
    tcpClients[freeIndex].lastHeartbeat = now;
    tcpClients[freeIndex].lastSend = 0;
    pendingBroadcast = true;

    logPrintln("[wifi] TCP client connected");
  }
}

void serviceTcpClients(unsigned long now) {
  handleNewTcpClients(now);

  bool forceBroadcast = pendingBroadcast;

  for (auto &slot : tcpClients) {
    if (!slot.active) {
      continue;
    }

    if (!slot.client.connected()) {
      disconnectClient(slot, "connection lost");
      continue;
    }

    while (slot.client.available() > 0) {
      int byteValue = slot.client.read();
      if (byteValue < 0) {
        break;
      }
      if (byteValue == kHeartbeatByte) {
        slot.lastHeartbeat = now;
      }
    }

    if ((now - slot.lastHeartbeat) > kHeartbeatTimeoutMs) {
      disconnectClient(slot, "heartbeat timeout");
      continue;
    }

    bool needSend =
        forceBroadcast || ((now - slot.lastSend) >= kBroadcastIntervalMs);
    if (!needSend) {
      continue;
    }

    if (!sendPayloadToClient(slot, now)) {
      disconnectClient(slot, "send failed");
      continue;
    }
  }

  if (forceBroadcast) {
    pendingBroadcast = false;
  }
}

String escapeJson(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 4);
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    switch (c) {
    case '"':
    case '\\':
      escaped += '\\';
      escaped += c;
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (static_cast<uint8_t>(c) < 0x20) {
        escaped += "\\u";
        char buf[5];
        snprintf(buf, sizeof(buf), "%04x", c);
        escaped += buf;
      } else {
        escaped += c;
      }
    }
  }
  return escaped;
}

bool loadCredentials() {
  if (!prefs.begin("wifi", true)) {
    return false;
  }
  String ssid = prefs.getString("ssid", "");
  String password = prefs.getString("password", "");
  prefs.end();

  storedCreds.ssid = ssid;
  storedCreds.password = password;
  storedCreds.valid = ssid.length() > 0;
  return storedCreds.valid;
}

void saveCredentials(const String &ssid, const String &password) {
  if (prefs.begin("wifi", false)) {
    prefs.putString("ssid", ssid);
    prefs.putString("password", password);
    prefs.end();
  }
  storedCreds.ssid = ssid;
  storedCreds.password = password;
  storedCreds.valid = ssid.length() > 0;
}

void setupWebRoutes();

const char *wifiStatusToString(wl_status_t status) {
  switch (status) {
  case WL_IDLE_STATUS:
    return "Ожидание";
  case WL_NO_SSID_AVAIL:
    return "SSID недоступен";
  case WL_SCAN_COMPLETED:
    return "Сканирование завершено";
  case WL_CONNECTED:
    return "Подключено";
  case WL_CONNECT_FAILED:
    return "Ошибка подключения";
  case WL_CONNECTION_LOST:
    return "Связь потеряна";
  case WL_DISCONNECTED:
    return "Отключено";
  default:
    return "Неизвестно";
  }
}


void startMdns() {
  if (mdnsStarted) {
    return;
  }
  if (!MDNS.begin("gps")) {
    logPrintln("[wifi] Failed to start mDNS responder");
    return;
  }
  MDNS.addService("http", "tcp", 80);
  MDNS.addService("gnss", "tcp", kGnssServerPort);
  mdnsStarted = true;
  logPrintln("[wifi] mDNS responder started as gps.local");
}

void stopMdns() {
  if (!mdnsStarted) {
    return;
  }
  MDNS.end();
  mdnsStarted = false;
  logPrintln("[wifi] mDNS responder stopped");
}

String htmlEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 4);
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    switch (c) {
    case '&':
      escaped += F("&amp;");
      break;
    case '<':
      escaped += F("&lt;");
      break;
    case '>':
      escaped += F("&gt;");
      break;
    case '"':
      escaped += F("&quot;");
      break;
    case '\'':
      escaped += F("&#39;");
      break;
    default:
      escaped += c;
    }
  }
  return escaped;
}

String floatToString(float value, uint8_t decimals = 2) {
  char buf[24];
  dtostrf(value, 0, decimals, buf);
  return String(buf);
}

void sendStationPage() {
  webServer.sendHeader("Content-Encoding", "gzip");
  webServer.sendHeader("Cache-Control", "no-cache");
  webServer.send_P(200, "text/html",
                   reinterpret_cast<const char *>(WEB_INDEX_HTML),
                   sizeof(WEB_INDEX_HTML));
}

void startAccessPoint(ApRequestSource source) {
  if (apActive) {
    return;
  }

  if (source == ApRequestSource::None) {
    source = ApRequestSource::Button;
  }

  ensureApSsid();

  bool apOnly = (source == ApRequestSource::Ble);
  if (apOnly || !storedCreds.valid) {
    WiFi.mode(WIFI_MODE_AP);
  } else {
    WiFi.mode(WIFI_MODE_APSTA);
  }

  if (apOnly) {
    stationConnecting = false;
  }

  logPrintf("[wifi] Starting access point '%s'\n", apSsid.c_str());
  WiFi.softAP(apSsid.c_str());
  delay(50);
  apIp = WiFi.softAPIP();

  dnsServer.start(53, "*", apIp);
  dnsRunning = true;

  apActive = true;
  activeApSource = source;
  scheduleApStopAt = 0;

  if (apCallback) {
    apCallback(true);
  }
  logPrintln("[wifi] Access point started");
}

void stopAccessPoint() {
  if (!apActive) {
    return;
  }

  if (dnsRunning) {
    dnsServer.stop();
    dnsRunning = false;
  }

  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_MODE_STA);

  apActive = false;
  activeApSource = ApRequestSource::None;
  scheduleApStopAt = 0;
  logPrintln("[wifi] Access point stopped");
  if (apCallback) {
    apCallback(false);
  }
}

String wifiManagerApSsid() {
  ensureApSsid();
  return apSsid;
}

bool ensureStationConnecting() {
  if (!storedCreds.valid) {
    return false;
  }
  if (apActive && activeApSource == ApRequestSource::Ble) {
    return false;
  }

  unsigned long now = millis();
  if (!stationConnecting ||
      (now - lastReconnectAttempt) > kReconnectIntervalMs) {
    logPrintf("[wifi] Attempting STA connection to '%s'\n",
              storedCreds.ssid.c_str());
    if (apActive) {
      WiFi.mode(WIFI_MODE_APSTA);
    } else {
      WiFi.mode(WIFI_MODE_STA);
    }
    WiFi.begin(storedCreds.ssid.c_str(), storedCreds.password.c_str());
    stationConnecting = true;
    lastReconnectAttempt = now;
    return true;
  }
  return false;
}

void handleRoot() {
  if (apActive) {
    webServer.sendHeader("Content-Encoding", "gzip");
    webServer.sendHeader("Cache-Control", "no-cache");
    webServer.send_P(200, "text/html",
                     reinterpret_cast<const char *>(WEB_PORTAL_HTML),
                     sizeof(WEB_PORTAL_HTML));
  } else {
    sendStationPage();
  }
}

void sendRedirect() {
  String location = "http://" + apIp.toString() + "/";
  webServer.sendHeader("Location", location, true);
  webServer.send(302, "text/plain", "");
}

void handleStatus() {
  String json = "{";
  json += "\"ap\":";
  json += apActive ? "true" : "false";
  json += ",\"connected\":";
  bool connected = WiFi.status() == WL_CONNECTED;
  json += connected ? "true" : "false";
  if (connected) {
    json += ",\"ssid\":\"";
    json += escapeJson(WiFi.SSID());
    json += "\"";
    json += ",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\"";
  }
  json += ",\"hasCredentials\":";
  json += storedCreds.valid ? "true" : "false";
  if (storedCreds.valid) {
    json += ",\"configuredSsid\":\"";
    json += escapeJson(storedCreds.ssid);
    json += "\"";
  }
  ensureApSsid();
  json += ",\"apSsid\":\"";
  json += escapeJson(apSsid);
  json += "\"";
  json += ",\"apIp\":\"";
  json += apIp.toString();
  json += "\"";
  json += ",\"build\":{";
  json += "\"version\":\"";
  json += BUILD_VERSION;
  json += "\",\"timestamp\":\"";
  json += BUILD_TIMESTAMP;
  json += "\"}";
  json += "}";
  webServer.send(200, "application/json", json);
}

void handleDeviceState() {
  ensureApSsid();
  String json = "{";

  json += "\"wifi\":{";
  json += "\"ap\":";
  json += apActive ? "true" : "false";
  json += ",\"apSsid\":\"";
  json += escapeJson(apSsid);
  json += "\"";
  json += ",\"apIp\":\"";
  json += apIp.toString();
  json += "\"";
  bool connected = WiFi.status() == WL_CONNECTED;
  json += ",\"connected\":";
  json += connected ? "true" : "false";
  if (connected) {
    json += ",\"ssid\":\"";
    json += escapeJson(WiFi.SSID());
    json += "\",\"ip\":\"";
    json += WiFi.localIP().toString();
    json += "\"";
  }
  json += "}";

  json += ",\"build\":{";
  json += "\"version\":\"";
  json += BUILD_VERSION;
  json += "\",\"timestamp\":\"";
  json += BUILD_TIMESTAMP;
  json += "\"}";

  json += ",\"ota\":{";
  json += "\"enabled\":";
  json += otaUpdatesEnabled() ? "true" : "false";
  json += ",\"inProgress\":";
  json += otaUpdateInProgress() ? "true" : "false";
  json += "}";

  json += ",\"nav\":{";
  json += "\"valid\":";
  json += navSnapshot.valid ? "true" : "false";
  if (navSnapshot.valid) {
    json += ",\"lat\":";
    json += floatToString(navSnapshot.latitude, 6);
    json += ",\"lon\":";
    json += floatToString(navSnapshot.longitude, 6);
    json += ",\"alt\":";
    json += floatToString(navSnapshot.altitude, 1);
    json += ",\"speed\":";
    json += floatToString(navSnapshot.speed, 2);
    json += ",\"heading\":";
    json += floatToString(navSnapshot.heading, 1);
    json += ",\"age\":";
    json += (millis() - navSnapshot.updatedAt) / 1000;
  }
  json += "}";

  json += ",\"fix\":{";
  json += "\"valid\":";
  json += statusSnapshot.valid ? "true" : "false";
  if (statusSnapshot.valid) {
    json += ",\"fix\":";
    json += statusSnapshot.fix ? "true" : "false";
    json += ",\"hdop\":";
    json += floatToString(statusSnapshot.hdop, 1);
    json += ",\"ttff\":";
    json += statusSnapshot.ttffSeconds;
    json += ",\"sats\":";
    json += statusSnapshot.satellites;
    json += ",\"signals\":\"";
    json += escapeJson(statusSnapshot.signals);
    json += "\"";
    json += ",\"age\":";
    json += (millis() - statusSnapshot.updatedAt) / 1000;
  }
  json += "}";

  json += "}";
  webServer.send(200, "application/json", json);
}

void handleNetworks() {
  if (!apActive) {
    webServer.send(403, "application/json", "[]");
    return;
  }
  int16_t networkCount = WiFi.scanNetworks(/*async=*/false, /*hidden=*/true);
  String json = "[";
  for (int16_t i = 0; i < networkCount; ++i) {
    if (i > 0) {
      json += ",";
    }
    json += "{\"ssid\":\"";
    json += escapeJson(WiFi.SSID(i));
    json += "\",\"rssi\":";
    json += WiFi.RSSI(i);
    json += ",\"secure\":";
    wifi_auth_mode_t auth = WiFi.encryptionType(i);
    json += (auth == WIFI_AUTH_OPEN) ? "false" : "true";
    json += "}";
  }
  json += "]";
  WiFi.scanDelete();
  webServer.send(200, "application/json", json);
}

void handleConfigure() {
  if (!apActive) {
    webServer.send(403, "text/plain",
                   "Настройка доступна только в режиме точки доступа");
    return;
  }
  if (!webServer.hasArg("ssid")) {
    webServer.send(400, "text/plain", "Не указан SSID");
    return;
  }
  String ssid = webServer.arg("ssid");
  String password = webServer.arg("password");
  ssid.trim();
  password.trim();
  if (ssid.isEmpty()) {
    webServer.send(400, "text/plain", "SSID не может быть пустым");
    return;
  }

  saveCredentials(ssid, password);
  logPrintf("[wifi] Credentials saved for '%s'\n", ssid.c_str());
  stationConnectPending = true;
  scheduleApStopAt = millis() + 5000;

  webServer.send(200, "text/plain",
                 "Данные сохранены. Устройство начнет подключение.");
}

void handleConnectivityCheck() {
  if (apActive) {
    sendRedirect();
  } else {
    webServer.send(200, "text/plain", "OK");
  }
}

void handleNotFound() {
  if (apActive) {
    sendRedirect();
  } else {
    webServer.sendHeader("Location", "/", true);
    webServer.send(302, "text/plain", "");
  }
}

void setupWebRoutes() {
  webServer.on("/", HTTP_ANY, handleRoot);
  webServer.on("/status", HTTP_GET, handleStatus);
  webServer.on("/api/state", HTTP_GET, handleDeviceState);
  webServer.on("/networks", HTTP_GET, handleNetworks);
  webServer.on("/configure", HTTP_POST, handleConfigure);
  webServer.on("/generate_204", HTTP_GET, handleConnectivityCheck);
  webServer.on("/gen_204", HTTP_GET, handleConnectivityCheck);
  webServer.on("/hotspot-detect.html", HTTP_GET, handleConnectivityCheck);
  webServer.on("/connecttest.txt", HTTP_GET, handleConnectivityCheck);
  webServer.onNotFound(handleNotFound);
}

} // namespace

void initWifiManager(ApStateCallback callback) {
  apCallback = callback;

  pinMode(WIFI_AP_BUTTON_PIN, INPUT_PULLUP);

  WiFi.persistent(false);
  WiFi.mode(WIFI_MODE_STA);

  logPrintln("[wifi] Initialising Wi-Fi manager...");
  setupWebRoutes();
  webServer.begin();
  webServerStarted = true;
  logPrintln("[wifi] HTTP server started on port 80");

  gnssTcpServer.begin();
  logPrintf("[wifi] GNSS TCP server listening on port %u\n", kGnssServerPort);

  loadCredentials();
  if (storedCreds.valid) {
    logPrintf("[wifi] Found stored credentials for '%s'\n",
              storedCreds.ssid.c_str());
  } else {
    logPrintln("[wifi] No stored Wi-Fi credentials");
  }
  lastWifiStatus = WiFi.status();
  logPrintf("[wifi] Initial STA status: %s\n",
            wifiStatusToString(lastWifiStatus));
  if (lastWifiStatus == WL_CONNECTED) {
    startMdns();
  }
  if (storedCreds.valid) {
    ensureStationConnecting();
  }

  if (apCallback) {
    apCallback(apActive);
  }
}

void requestApMode(ApRequestSource source) {
  if (apActive) {
    return;
  }
  if (source == ApRequestSource::None) {
    source = ApRequestSource::Button;
  }
  pendingApSource = source;
  const char *sourceLabel = "auto";
  switch (source) {
  case ApRequestSource::Ble:
    sourceLabel = "BLE";
    break;
  case ApRequestSource::Button:
    sourceLabel = "button";
    break;
  default:
    break;
  }
  logPrintf("[wifi] Queuing AP start request (%s)\n", sourceLabel);
  apRequested = true;
}

void updateWifiManager() {
  bool buttonPressed = digitalRead(WIFI_AP_BUTTON_PIN) == LOW;
  unsigned long now = millis();
  if (buttonPressed) {
    if (buttonPressStarted == 0) {
      buttonPressStarted = now;
    } else if (!buttonTriggered &&
               (now - buttonPressStarted) >= WIFI_AP_TRIGGER_MS) {
      buttonTriggered = true;
      logPrintln("[wifi] AP requested via button hold");
      requestApMode(ApRequestSource::Button);
    }
  } else {
    buttonPressStarted = 0;
    buttonTriggered = false;
  }

  wl_status_t status = WiFi.status();
  if (status != lastWifiStatus) {
    logPrintf("[wifi] STA status -> %s\n", wifiStatusToString(status));
    if (status == WL_CONNECTED) {
      logPrintf("[wifi] Connected to '%s' with IP %s\n", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str());
      startMdns();
      stationConnecting = false;
    } else {
      stopMdns();
    }
    lastWifiStatus = status;
  }

  if (apRequested && !apActive) {
    apRequested = false;
    ApRequestSource source = pendingApSource;
    if (source == ApRequestSource::None) {
      source = ApRequestSource::Button;
    }
    pendingApSource = ApRequestSource::None;
    startAccessPoint(source);
  }

  if (apActive && dnsRunning) {
    dnsServer.processNextRequest();
  }

  if (apActive && scheduleApStopAt && now >= scheduleApStopAt) {
    stopAccessPoint();
    scheduleApStopAt = 0;
  }

  if (stationConnectPending) {
    if (apActive && activeApSource == ApRequestSource::Ble) {
      // Defer connection attempts until BLE-managed AP session ends.
    } else if (ensureStationConnecting()) {
      stationConnectPending = false;
    } else if (!storedCreds.valid) {
      stationConnectPending = false;
    }
  }

  if (storedCreds.valid) {
    if (status != WL_CONNECTED) {
      ensureStationConnecting();
    } else if (status == WL_CONNECTED) {
      stationConnecting = false;
    }
  } else if (status == WL_CONNECTED) {
    stationConnecting = false;
  }

  if (webServerStarted) {
    webServer.handleClient();
  }

  serviceTcpClients(now);
}

void wifiManagerHandleBleRequest(bool enable) {
  if (enable) {
    if (apActive) {
      logPrintln("[wifi] BLE requested AP enable, already active");
      return;
    }
    logPrintln("[wifi] AP requested via BLE");
    requestApMode(ApRequestSource::Ble);
    return;
  }

  logPrintln("[wifi] BLE requested AP disable");

  if (apRequested && !apActive) {
    logPrintln("[wifi] Cancelling pending AP start request");
    apRequested = false;
    pendingApSource = ApRequestSource::None;
  }

  if (!apActive) {
    if (storedCreds.valid) {
      ensureStationConnecting();
    }
    return;
  }

  stopAccessPoint();

  if (storedCreds.valid) {
    ensureStationConnecting();
  }
}

bool wifiManagerIsApActive() { return apActive; }

bool wifiManagerIsConnected() { return WiFi.status() == WL_CONNECTED; }

bool wifiManagerHasCredentials() { return storedCreds.valid; }

WebServer *wifiManagerHttpServer() { return &webServer; }

WifiStatusInfo wifiManagerGetStatus() {
  WifiStatusInfo info;
  info.apActive = apActive;
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    info.state = WifiConnectionState::Connected;
    String ip = WiFi.localIP().toString();
    if (ip.length() > 0 && ip != "0.0.0.0") {
      info.ip = ip;
    }
  } else if (apActive) {
    info.ip = WiFi.softAPIP().toString();
    if (info.ip == "0.0.0.0") {
      info.ip.clear();
    }
  } else if (stationConnecting) {
    info.state = WifiConnectionState::Connecting;
  } else {
    info.state = WifiConnectionState::Disconnected;
  }
  return info;
}

void WifiManagerPublisher::publishNavData(const NavDataSample &sample) {
  if (!gnssStreamingEnabled) {
    return;
  }
  navSnapshot.valid = true;
  navSnapshot.latitude = sample.latitude;
  navSnapshot.longitude = sample.longitude;
  navSnapshot.heading = sample.heading;
  navSnapshot.speed = sample.speed;
  navSnapshot.altitude = sample.altitude;
  unsigned long now = millis();
  navSnapshot.updatedAt = now;
  navSnapshot.timestampMs = static_cast<int64_t>(now);
  markPayloadDirty();
}

void WifiManagerPublisher::publishSystemStatus(
    const SystemStatusSample &sample) {
  if (!gnssStreamingEnabled) {
    return;
  }
  statusSnapshot.valid = true;
  statusSnapshot.fix = sample.fix != 0;
  statusSnapshot.hdop = sample.hdop;
  statusSnapshot.signals = sample.signalsJson;
  statusSnapshot.ttffSeconds = sample.ttffSeconds;
  statusSnapshot.satellites = sample.satellites;
  statusSnapshot.updatedAt = millis();
  markPayloadDirty();
}

void wifiManagerSetGnssStreamingEnabled(bool enabled) {
  if (gnssStreamingEnabled == enabled) {
    return;
  }
  gnssStreamingEnabled = enabled;
  if (!enabled) {
    navSnapshot = NavSnapshot();
    statusSnapshot = StatusSnapshot();
    pbPayloadValid = false;
    pbPayloadDirty = true;
    pendingBroadcast = true;
  } else {
    markPayloadDirty();
  }
}

NavDataPublisher *wifiManagerNavPublisher() { return &gWifiPublisher; }

SystemStatusPublisher *wifiManagerStatusPublisher() {
  return &gWifiPublisher;
}
