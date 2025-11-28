#include "gps_controller.h"

#include "gps_ble.h"
#include "gps_config.h"
#include "gps_serial_control.h"
#include "led_status.h"
#include "logger.h"
#include "system_mode.h"
#include "ubx_command_set.h"

#include <Arduino.h>
#include <Preferences.h>
#include <ctype.h>
#include <iarduino_GPS_NMEA.h>
#include <string>

namespace {
HardwareSerial gpsSerial(1);
iarduino_GPS_NMEA gpsParser;
constexpr const char *kGpsPrefsNamespace = "gpscfg";
constexpr const char *kGpsBaudKey = "baud";
constexpr const char *kGpsProfileKey = "profile";
constexpr const char *kGpsSettingsProfileKey = "cfgsel";
constexpr const char *kGpsCustomProfileKey = "custprof";
constexpr const char *kGpsCustomSettingsKey = "custset";
constexpr UbxConfigProfile kDefaultUbxProfile = UbxConfigProfile::FullSystems;
constexpr UbxSettingsProfile kDefaultUbxSettingsProfile =
    UbxSettingsProfile::DefaultRamBbr;
constexpr size_t kUbxPayloadBufferSize = 196;
constexpr uint32_t kUbxAckTimeoutMs = 600;
constexpr uint32_t kUbxResponseTimeoutMs = 1200;
constexpr uint32_t kUbxInterCommandDelayMs = 30;
constexpr uint32_t kUbxDrainWindowMs = 50;
constexpr uint32_t kUbxStartupDelayMs = 250;
constexpr uint8_t kUbxValgetLayerRam = 0;
constexpr uint32_t kUbxKeyMask = 0xFFFFFFF8u;

struct UbxFrame {
  uint8_t msgClass = 0;
  uint8_t msgId = 0;
  uint16_t payloadSize = 0;
  uint16_t payloadStored = 0;
  uint8_t payload[kUbxPayloadBufferSize] = {};
};

bool waitForSpecificFrame(uint8_t desiredClass, uint8_t desiredId,
                          UbxFrame &frame, uint32_t timeoutMs);

void logUbxFrame(const char *label, const UbxFrame &frame) {
  const char *tag = label ? label : "UBX";
  logPrintf("[gps] %s: class=0x%02X id=0x%02X len=%u\n", tag, frame.msgClass,
            frame.msgId, static_cast<unsigned>(frame.payloadSize));
  if (!frame.payloadStored)
    return;
  constexpr size_t kMaxDump = 32;
  size_t dump = frame.payloadStored < kMaxDump ? frame.payloadStored : kMaxDump;
  char hexBuf[kMaxDump * 3 + 4];
  size_t pos = 0;
  for (size_t i = 0; i < dump && pos + 4 < sizeof(hexBuf); ++i) {
    int written =
        snprintf(&hexBuf[pos], sizeof(hexBuf) - pos, "%02X ", frame.payload[i]);
    if (written <= 0)
      break;
    pos += static_cast<size_t>(written);
  }
  if (pos < sizeof(hexBuf)) {
    hexBuf[pos] = '\0';
  } else {
    hexBuf[sizeof(hexBuf) - 1] = '\0';
  }
  logPrintf("[gps] %s payload: %s%s\n", tag, hexBuf,
            (frame.payloadStored > dump) ? "..." : "");
}

int hexDigitValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return 10 + static_cast<int>(c - 'a');
  if (c >= 'A' && c <= 'F')
    return 10 + static_cast<int>(c - 'A');
  return -1;
}

bool parseUbxHexCommand(const std::string &value, uint8_t *bufferOut,
                        size_t &sizeOut, std::string &errorOut) {
  sizeOut = 0;
  if (!bufferOut) {
    errorOut = "no buffer";
    return false;
  }

  uint8_t temp[kMaxUbxCustomCommandSize];
  size_t count = 0;
  bool highNibble = true;
  uint8_t current = 0;

  for (char c : value) {
    if (isspace(static_cast<unsigned char>(c)))
      continue;
    int digit = hexDigitValue(c);
    if (digit < 0) {
      errorOut = "non-hex character";
      return false;
    }
    if (highNibble) {
      current = static_cast<uint8_t>(digit << 4);
      highNibble = false;
      continue;
    }
    current = static_cast<uint8_t>(current | static_cast<uint8_t>(digit));
    if (count >= kMaxUbxCustomCommandSize) {
      errorOut = "command too long";
      return false;
    }
    temp[count++] = current;
    highNibble = true;
  }

  if (!highNibble) {
    errorOut = "odd number of hex digits";
    return false;
  }
  if (count == 0) {
    errorOut = "empty command";
    return false;
  }
  if (count < 8) {
    errorOut = "command too short";
    return false;
  }
  if (temp[0] != 0xB5 || temp[1] != 0x62) {
    errorOut = "missing UBX sync";
    return false;
  }

  uint16_t payloadLen =
      static_cast<uint16_t>(temp[4]) | (static_cast<uint16_t>(temp[5]) << 8);
  size_t expectedSize = static_cast<size_t>(payloadLen) + 8;
  if (expectedSize != count) {
    errorOut = "length mismatch";
    return false;
  }

  uint8_t ckA = 0;
  uint8_t ckB = 0;
  for (size_t i = 2; i < count - 2; ++i) {
    ckA = static_cast<uint8_t>(ckA + temp[i]);
    ckB = static_cast<uint8_t>(ckB + ckA);
  }
  if (ckA != temp[count - 2] || ckB != temp[count - 1]) {
    errorOut = "checksum mismatch";
    return false;
  }

  memcpy(bufferOut, temp, count);
  sizeOut = count;
  return true;
}

std::string formatUbxHexCommand(const uint8_t *data, size_t size) {
  if (!data || size == 0)
    return std::string();

  std::string result;
  result.reserve(size * 3);
  for (size_t i = 0; i < size; ++i) {
    char chunk[4];
    int written = snprintf(chunk, sizeof(chunk), "%02X", data[i]);
    if (written > 0) {
      result.append(chunk);
    }
    if (i + 1 < size) {
      result.push_back(' ');
    }
  }
  return result;
}

void persistCustomCommand(const char *key, const uint8_t *data, size_t size) {
  if (!data || size == 0)
    return;
  Preferences prefs;
  if (prefs.begin(kGpsPrefsNamespace, false)) {
    prefs.putBytes(key, data, size);
    prefs.end();
  }
}

bool sendUbxMessage(uint8_t msgClass, uint8_t msgId, const uint8_t *payload,
                    size_t payloadSize) {
  if (!payload && payloadSize > 0) {
    return false;
  }
  size_t bytesWritten = 0;
  bytesWritten += gpsSerial.write(0xB5);
  bytesWritten += gpsSerial.write(0x62);

  uint8_t lenLow = static_cast<uint8_t>(payloadSize & 0xFFu);
  uint8_t lenHigh = static_cast<uint8_t>((payloadSize >> 8) & 0xFFu);

  auto updateChecksum = [](uint8_t byte, uint8_t &ckA, uint8_t &ckB) {
    ckA = static_cast<uint8_t>(ckA + byte);
    ckB = static_cast<uint8_t>(ckB + ckA);
  };

  uint8_t ckA = 0;
  uint8_t ckB = 0;

  bytesWritten += gpsSerial.write(msgClass);
  updateChecksum(msgClass, ckA, ckB);

  bytesWritten += gpsSerial.write(msgId);
  updateChecksum(msgId, ckA, ckB);

  bytesWritten += gpsSerial.write(lenLow);
  updateChecksum(lenLow, ckA, ckB);

  bytesWritten += gpsSerial.write(lenHigh);
  updateChecksum(lenHigh, ckA, ckB);

  for (size_t i = 0; i < payloadSize; ++i) {
    bytesWritten += gpsSerial.write(payload[i]);
    updateChecksum(payload[i], ckA, ckB);
  }

  bytesWritten += gpsSerial.write(ckA);
  bytesWritten += gpsSerial.write(ckB);
  gpsSerial.flush();

  size_t expected = 2 + 4 + payloadSize + 2;
  return bytesWritten == expected;
}

bool requestUbxConfigValue(uint32_t key, uint8_t &valueOut, uint8_t layer) {
  uint8_t payload[8];
  payload[0] = 0; // version
  payload[1] = layer;
  payload[2] = 0;
  payload[3] = 0;
  uint32_t maskedKey = key;
  payload[4] = static_cast<uint8_t>(maskedKey & 0xFFu);
  payload[5] = static_cast<uint8_t>((maskedKey >> 8) & 0xFFu);
  payload[6] = static_cast<uint8_t>((maskedKey >> 16) & 0xFFu);
  payload[7] = static_cast<uint8_t>((maskedKey >> 24) & 0xFFu);
  if (!sendUbxMessage(0x06, 0x8B, payload, sizeof(payload))) {
    return false;
  }

  UbxFrame frame;
  if (!waitForSpecificFrame(0x06, 0x8B, frame, kUbxResponseTimeoutMs)) {
    return false;
  }
  logUbxFrame("UBX VALGET", frame);
  if (frame.payloadStored < 9) {
    return false;
  }

  uint32_t responseKey = static_cast<uint32_t>(frame.payload[4]) |
                         (static_cast<uint32_t>(frame.payload[5]) << 8) |
                         (static_cast<uint32_t>(frame.payload[6]) << 16) |
                         (static_cast<uint32_t>(frame.payload[7]) << 24);
  if ((responseKey & kUbxKeyMask) != (maskedKey & kUbxKeyMask)) {
    return false;
  }
  valueOut = frame.payload[8];
  return true;
}

void drainGpsSerialInput() {
  unsigned long start = millis();
  while (millis() - start < kUbxDrainWindowMs) {
    while (gpsSerial.available() > 0) {
      gpsSerial.read();
      start = millis();
    }
    delay(1);
  }
}

bool sendUbxCommand(const UbxBinaryCommand &command) {
  if (!command.data || command.size < 8) {
    logPrintln("[gps] UBX command is not valid, skipping");
    return false;
  }
  size_t written = gpsSerial.write(command.data, command.size);
  gpsSerial.flush();
  return written == command.size;
}

bool readUbxFrame(UbxFrame &frame, uint32_t timeoutMs) {
  enum class ParserState {
    Sync1,
    Sync2,
    Class,
    Id,
    Len1,
    Len2,
    Payload,
    CkA,
    CkB
  };
  ParserState state = ParserState::Sync1;
  uint8_t ckA = 0;
  uint8_t ckB = 0;
  uint16_t payloadLen = 0;
  uint16_t payloadRead = 0;
  frame.payloadStored = 0;
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    if (gpsSerial.available() == 0) {
      delay(1);
      continue;
    }
    uint8_t value = static_cast<uint8_t>(gpsSerial.read());
    switch (state) {
    case ParserState::Sync1:
      if (value == 0xB5) {
        state = ParserState::Sync2;
      }
      break;
    case ParserState::Sync2:
      if (value == 0x62) {
        state = ParserState::Class;
      } else {
        state = ParserState::Sync1;
      }
      break;
    case ParserState::Class:
      frame.msgClass = value;
      ckA = value;
      ckB = ckA;
      state = ParserState::Id;
      break;
    case ParserState::Id:
      frame.msgId = value;
      ckA += value;
      ckB += ckA;
      state = ParserState::Len1;
      break;
    case ParserState::Len1:
      payloadLen = value;
      ckA += value;
      ckB += ckA;
      state = ParserState::Len2;
      break;
    case ParserState::Len2:
      payloadLen |= static_cast<uint16_t>(value) << 8;
      frame.payloadSize = payloadLen;
      ckA += value;
      ckB += ckA;
      payloadRead = 0;
      frame.payloadStored = 0;
      state = (payloadLen == 0) ? ParserState::CkA : ParserState::Payload;
      break;
    case ParserState::Payload:
      if (payloadRead < kUbxPayloadBufferSize) {
        frame.payload[payloadRead] = value;
        frame.payloadStored = payloadRead + 1;
      }
      payloadRead++;
      ckA += value;
      ckB += ckA;
      if (payloadRead >= payloadLen) {
        state = ParserState::CkA;
      }
      break;
    case ParserState::CkA:
      if (value != ckA) {
        state = ParserState::Sync1;
        ckA = ckB = 0;
        payloadLen = payloadRead = 0;
      } else {
        state = ParserState::CkB;
      }
      break;
    case ParserState::CkB:
      if (value != ckB) {
        state = ParserState::Sync1;
        ckA = ckB = 0;
        payloadLen = payloadRead = 0;
      } else {
        return true;
      }
      break;
    }
  }
  return false;
}

bool waitForSpecificFrame(uint8_t desiredClass, uint8_t desiredId,
                          UbxFrame &frame, uint32_t timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    uint32_t elapsed = millis() - start;
    uint32_t remaining = (elapsed >= timeoutMs) ? 0 : (timeoutMs - elapsed);
    if (remaining == 0) {
      remaining = 1;
    }
    if (!readUbxFrame(frame, remaining)) {
      return false;
    }
    if (frame.msgClass == desiredClass && frame.msgId == desiredId) {
      return true;
    }
  }
  return false;
}

bool waitForUbxAck(uint8_t msgClass, uint8_t msgId, uint32_t timeoutMs) {
  UbxFrame frame;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    uint32_t elapsed = millis() - start;
    uint32_t remaining = (elapsed >= timeoutMs) ? 0 : (timeoutMs - elapsed);
    if (remaining == 0) {
      remaining = 1;
    }
    if (!readUbxFrame(frame, remaining)) {
      return false;
    }
    if (frame.msgClass == 0x05 && frame.msgId == 0x01 &&
        frame.payloadStored >= 2) {
      if (frame.payload[0] == msgClass && frame.payload[1] == msgId) {
        return true;
      }
    }
    if (frame.msgClass == 0x05 && frame.msgId == 0x00 &&
        frame.payloadStored >= 2) {
      if (frame.payload[0] == msgClass && frame.payload[1] == msgId) {
        return false;
      }
    }
  }
  return false;
}

bool sendUbxCommandExpectAck(const UbxBinaryCommand &command) {
  if (!sendUbxCommand(command)) {
    return false;
  }
  uint8_t msgClass = command.data[2];
  uint8_t msgId = command.data[3];
  return waitForUbxAck(msgClass, msgId, kUbxAckTimeoutMs);
}

bool runUbxSequence(const UbxCommandSequence &sequence, const char *label) {
  const char *stage = label ? label : "sequence";
  if (!sequence.commands || sequence.length == 0) {
    logPrintf("[gps] UBX %s: skipped (no commands)\n", stage);
    return true;
  }
  logPrintf("[gps] UBX %s: running %u command(s)\n", stage,
            static_cast<unsigned>(sequence.length));
  for (size_t i = 0; i < sequence.length; ++i) {
    const UbxBinaryCommand &command = sequence.commands[i];
    if (!command.data || command.size < 8) {
      logPrintf("[gps] UBX %s: command %u is invalid\n", stage,
                static_cast<unsigned>(i));
      return false;
    }
    if (!sendUbxCommandExpectAck(command)) {
      logPrintf("[gps] UBX %s: command %u failed (no ACK)\n", stage,
                static_cast<unsigned>(i));
      return false;
    }
    delay(kUbxInterCommandDelayMs);
  }
  logPrintf("[gps] UBX %s: completed\n", stage);
  return true;
}

bool waitForUbxResponse(uint8_t expectedClass, uint8_t expectedId,
                        uint32_t timeoutMs) {
  UbxFrame frame;
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    uint32_t elapsed = millis() - start;
    uint32_t remaining = (elapsed >= timeoutMs) ? 0 : (timeoutMs - elapsed);
    if (remaining == 0) {
      remaining = 1;
    }
    if (!readUbxFrame(frame, remaining)) {
      return false;
    }
    if (frame.msgClass == expectedClass && frame.msgId == expectedId) {
      logUbxFrame("UBX response", frame);
      return true;
    }
  }
  return false;
}

bool probeUbxLink() {
  if (!kUbxPingCommand.data || kUbxPingCommand.size < 8) {
    logPrintln("[gps] UBX ping command is not configured");
    return false;
  }
  if (!sendUbxCommand(kUbxPingCommand)) {
    logPrintln("[gps] Failed to send UBX ping command");
    return false;
  }
  if (waitForUbxResponse(kUbxPingCommand.data[2], kUbxPingCommand.data[3],
                         kUbxResponseTimeoutMs)) {
    logPrintln("[gps] UBX ping response received");
    return true;
  }
  logPrintln("[gps] UBX ping timed out");
  return false;
}
} // namespace

GpsController &gpsController() {
  static GpsController instance;
  return instance;
}

void GpsController::begin() {
  state = GpsRuntimeState{};
  state.bootMillis = millis();
  gpsSerialBaudValue = loadStoredGpsBaud();
  currentProfile = loadStoredUbxProfile();
  currentSettingsProfile = loadStoredUbxSettingsProfile();
  loadStoredCustomCommands();
  prevFix = 255;
  prevHdop10 = -1;
  prevStrong = prevMedium = prevWeak = 255;

  pinMode(GPS_EN, OUTPUT);
  digitalWrite(GPS_EN, HIGH);

  applyUbxProfile(currentProfile);
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

UbxConfigProfile GpsController::ubxProfile() const { return currentProfile; }

UbxSettingsProfile GpsController::ubxSettingsProfile() const {
  return currentSettingsProfile;
}

bool GpsController::applyUbxProfile(UbxConfigProfile profile) {
  (void)profile;
  if (state.passthroughActive) {
    logPrintln("[gps] Cannot apply UBX profile while in passthrough mode");
    return false;
  }
  configureGpsSerial(false, true);
  bool success = runUbxStartupSequence();
  if (!state.passthroughActive) {
    configureGpsSerial(true, true);
    resetNavigationState();
  }
  return success;
}

bool GpsController::setUbxProfile(UbxConfigProfile profile) {
  size_t index = static_cast<size_t>(profile);
  if (index >= kUbxConfigProfileCount) {
    profile = kDefaultUbxProfile;
  }
  if (state.passthroughActive) {
    logPrintln("[gps] Cannot change UBX profile in passthrough mode");
    return false;
  }
  if (profile != currentProfile) {
    logPrintf("[gps] UBX profile -> %s\n", ubxProfileName(profile));
    currentProfile = profile;
    persistUbxProfile(profile);
  }
  bool success = applyUbxProfile(profile);
  updateUbxProfileCharacteristic(profile);
  return success;
}

bool GpsController::setUbxSettingsProfile(UbxSettingsProfile profile) {
  size_t index = static_cast<size_t>(profile);
  if (index >= kUbxSettingsProfileCount) {
    profile = kDefaultUbxSettingsProfile;
  }
  if (state.passthroughActive) {
    logPrintln("[gps] Cannot change UBX settings in passthrough mode");
    return false;
  }
  if (profile != currentSettingsProfile) {
    logPrintf("[gps] UBX settings -> %s\n", ubxSettingsProfileName(profile));
    currentSettingsProfile = profile;
    persistUbxSettingsProfile(profile);
  }
  bool success = applyUbxProfile(currentProfile);
  updateUbxSettingsProfileCharacteristic(profile);
  return success;
}

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

bool GpsController::runUbxStartupSequence() {
  const char *profileLabel = ubxProfileName(currentProfile);
  const char *settingsLabel = ubxSettingsProfileName(currentSettingsProfile);
  UbxConfigProfile verifyProfile = currentProfile;
  bool customProfileLoaded = currentProfile == UbxConfigProfile::Custom &&
                             hasCustomUbxProfileCommand();
  bool customSettingsLoaded =
      currentSettingsProfile == UbxSettingsProfile::CustomRam &&
      hasCustomUbxSettingsCommand();

  logPrintf("[gps] UBX startup sequence begin (%s, %s)\n", profileLabel,
            settingsLabel);
  if (kUbxStartupDelayMs > 0) {
    delay(kUbxStartupDelayMs);
  }
  drainGpsSerialInput();

  bool disableOk = runUbxSequence(kUbxDisableNmeaSequence, "disable NMEA");
  bool linkOk = probeUbxLink();
  if (currentSettingsProfile == UbxSettingsProfile::CustomRam &&
      !customSettingsLoaded) {
    logPrintln("[gps] Custom UBX settings selected, but no command is stored "
               "(fallback)");
  }
  bool settingsOk = runUbxSequence(ubxSettingsSequence(currentSettingsProfile),
                                   settingsLabel);
  if (currentProfile == UbxConfigProfile::Custom && !customProfileLoaded) {
    logPrintln("[gps] Custom UBX profile selected, but no command is stored "
               "(fallback)");
    verifyProfile = kDefaultUbxProfile;
  }
  bool profileOk =
      runUbxSequence(ubxProfileSequence(currentProfile), profileLabel);
  bool verifyOk = verifyUbxProfile(verifyProfile);
  bool enableOk = runUbxSequence(kUbxEnableNmeaSequence, "enable NMEA");

  drainGpsSerialInput();

  state.ubxLinkOk = linkOk && verifyOk;
  state.ubxConfigured = state.ubxLinkOk && settingsOk && profileOk;

  bool success =
      disableOk && linkOk && settingsOk && profileOk && verifyOk && enableOk;
  if (success) {
    logPrintln("[gps] UBX startup sequence completed");
  } else {
    logPrintln("[gps] UBX startup sequence failed");
  }
  return success;
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

UbxConfigProfile GpsController::loadStoredUbxProfile() {
  Preferences prefs;
  uint8_t stored = static_cast<uint8_t>(kDefaultUbxProfile);
  if (prefs.begin(kGpsPrefsNamespace, true)) {
    stored = prefs.getUChar(kGpsProfileKey, stored);
    prefs.end();
  }
  if (stored >= kUbxConfigProfileCount) {
    stored = static_cast<uint8_t>(kDefaultUbxProfile);
  }
  return static_cast<UbxConfigProfile>(stored);
}

void GpsController::persistUbxProfile(UbxConfigProfile profile) {
  Preferences prefs;
  if (prefs.begin(kGpsPrefsNamespace, false)) {
    prefs.putUChar(kGpsProfileKey, static_cast<uint8_t>(profile));
    prefs.end();
  }
}

UbxSettingsProfile GpsController::loadStoredUbxSettingsProfile() {
  Preferences prefs;
  uint8_t stored = static_cast<uint8_t>(kDefaultUbxSettingsProfile);
  if (prefs.begin(kGpsPrefsNamespace, true)) {
    stored = prefs.getUChar(kGpsSettingsProfileKey, stored);
    prefs.end();
  }
  if (stored >= kUbxSettingsProfileCount) {
    stored = static_cast<uint8_t>(kDefaultUbxSettingsProfile);
  }
  return static_cast<UbxSettingsProfile>(stored);
}

void GpsController::persistUbxSettingsProfile(UbxSettingsProfile profile) {
  Preferences prefs;
  if (prefs.begin(kGpsPrefsNamespace, false)) {
    prefs.putUChar(kGpsSettingsProfileKey, static_cast<uint8_t>(profile));
    prefs.end();
  }
}

void GpsController::loadStoredCustomCommands() {
  Preferences prefs;
  if (!prefs.begin(kGpsPrefsNamespace, true)) {
    return;
  }

  auto load = [&](const char *key, bool (*setter)(const uint8_t *, size_t),
                  const char *label) {
    size_t length = prefs.getBytesLength(key);
    if (length == 0)
      return;
    if (length > kMaxUbxCustomCommandSize) {
      logPrintf("[gps] Stored %s command too large (%u bytes), skipping\n",
                label, static_cast<unsigned>(length));
      return;
    }
    uint8_t buffer[kMaxUbxCustomCommandSize];
    size_t read = prefs.getBytes(key, buffer, length);
    if (read != length) {
      logPrintf("[gps] Failed to read %s command from NVS\n", label);
      return;
    }
    if (!setter(buffer, length)) {
      logPrintf("[gps] Failed to restore %s command\n", label);
    }
  };

  load(kGpsCustomProfileKey, setCustomUbxProfileCommand, "custom profile");
  load(kGpsCustomSettingsKey, setCustomUbxSettingsCommand, "custom settings");
  prefs.end();
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

  if (state.navUpdateCounter >= 5) {
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
  if (!state.ubxLinkOk) {
    return STATUS_NO_MODEM;
  }
  if (!fix || activeSatellites < 4) {
    return STATUS_NO_FIX;
  }
  if (fix && activeSatellites >= 4) {
    return STATUS_FIX_SYNC;
  }
  return STATUS_READY;
}

bool GpsController::verifyUbxProfile(UbxConfigProfile profile) {
  size_t entryCount = 0;
  const UbxKeyValue *entries = ubxProfileValidationTargets(profile, entryCount);
  if (!entries || entryCount == 0) {
    return true;
  }
  bool allGood = true;
  for (size_t i = 0; i < entryCount; ++i) {
    uint8_t value = 0;
    if (!requestUbxConfigValue(entries[i].key, value, kUbxValgetLayerRam)) {
      logPrintf("[gps] UBX verify failed to read key 0x%08lX\n",
                static_cast<unsigned long>(entries[i].key));
      allGood = false;
      continue;
    }
    if (value != entries[i].value) {
      logPrintf("[gps] UBX verify mismatch key 0x%08lX expected %u got %u\n",
                static_cast<unsigned long>(entries[i].key),
                static_cast<unsigned>(entries[i].value),
                static_cast<unsigned>(value));
      allGood = false;
    }
  }
  if (allGood) {
    logPrintf("[gps] UBX verify OK for %s\n", ubxProfileName(profile));
  }
  return allGood;
}

namespace {
bool storeCustomCommandFromHex(const std::string &value, bool settings) {
  const char *label = settings ? "settings" : "profile";
  uint8_t buffer[kMaxUbxCustomCommandSize];
  size_t size = 0;
  std::string error;
  if (!parseUbxHexCommand(value, buffer, size, error)) {
    logPrintf("[gps] Failed to parse custom %s command: %s\n", label,
              error.c_str());
    return false;
  }

  bool stored = settings ? setCustomUbxSettingsCommand(buffer, size)
                         : setCustomUbxProfileCommand(buffer, size);
  if (!stored) {
    logPrintf("[gps] Failed to store custom %s command\n", label);
    return false;
  }

  persistCustomCommand(settings ? kGpsCustomSettingsKey : kGpsCustomProfileKey,
                       buffer, size);
  logPrintf("[gps] Custom %s command saved (%u bytes)\n", label,
            static_cast<unsigned>(size));
  return true;
}

std::string currentCustomCommandHex(bool settings) {
  uint8_t buffer[kMaxUbxCustomCommandSize];
  size_t size = settings ? copyCustomUbxSettingsCommand(buffer, sizeof(buffer))
                         : copyCustomUbxProfileCommand(buffer, sizeof(buffer));
  return formatUbxHexCommand(buffer, size);
}
} // namespace

uint32_t getGpsSerialBaud() { return gpsController().baud(); }

bool setGpsSerialBaud(uint32_t baud) { return gpsController().setBaud(baud); }

UbxConfigProfile getGpsUbxProfile() { return gpsController().ubxProfile(); }

bool setGpsUbxProfile(UbxConfigProfile profile) {
  return gpsController().setUbxProfile(profile);
}

UbxSettingsProfile getGpsUbxSettingsProfile() {
  return gpsController().ubxSettingsProfile();
}

bool setGpsUbxSettingsProfile(UbxSettingsProfile profile) {
  return gpsController().setUbxSettingsProfile(profile);
}

bool setGpsCustomProfileCommand(const std::string &hex) {
  bool stored = storeCustomCommandFromHex(hex, false);
  if (!stored)
    return false;
  if (getGpsUbxProfile() == UbxConfigProfile::Custom) {
    return setGpsUbxProfile(UbxConfigProfile::Custom);
  }
  return true;
}

bool setGpsCustomSettingsCommand(const std::string &hex) {
  bool stored = storeCustomCommandFromHex(hex, true);
  if (!stored)
    return false;
  if (getGpsUbxSettingsProfile() == UbxSettingsProfile::CustomRam) {
    return setGpsUbxSettingsProfile(UbxSettingsProfile::CustomRam);
  }
  return true;
}

std::string getGpsCustomProfileCommand() {
  return currentCustomCommandHex(false);
}

std::string getGpsCustomSettingsCommand() {
  return currentCustomCommandHex(true);
}
