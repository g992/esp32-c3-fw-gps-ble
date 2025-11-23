#include "ota_service.h"

#include <Arduino.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include "firmware_app.h"
#include "logger.h"

namespace {

constexpr size_t kMaxChunkPayload = 480;
constexpr size_t kStatusBufferSize = 192;
constexpr size_t kProgressIntervalBytes = 16384;
constexpr size_t kValidationReadSize = 1024;

struct OtaSession {
  bool active = false;
  size_t imageSize = 0;
  size_t received = 0;
  size_t lastProgressNotified = 0;
  uint32_t nextOffset = 0;
  uint32_t expectedCrc32 = 0;
  uint8_t expectedSha256[32] = {};
  esp_ota_handle_t handle = 0;
  const esp_partition_t *partition = nullptr;
  bool shaCtxInitialized = false;
  mbedtls_sha256_context shaCtx;
  uint32_t crcAccumulator = 0xFFFFFFFFu;

  void reset() {
    if (handle != 0) {
      esp_ota_abort(handle);
      handle = 0;
    }
    if (shaCtxInitialized) {
      mbedtls_sha256_free(&shaCtx);
      shaCtxInitialized = false;
    }
    active = false;
    imageSize = 0;
    received = 0;
    lastProgressNotified = 0;
    nextOffset = 0;
    expectedCrc32 = 0;
    memset(expectedSha256, 0, sizeof(expectedSha256));
    partition = nullptr;
    crcAccumulator = 0xFFFFFFFFu;
  }
} gSession;

NimBLECharacteristic *gControlChar = nullptr;
NimBLECharacteristic *gDataChar = nullptr;
NimBLECharacteristic *gStatusChar = nullptr;

uint32_t gCrcTable[256];
bool gCrcTableReady = false;

class OtaControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) override;
};

class OtaDataCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic) override;
};

OtaControlCallbacks gControlCallbacks;
OtaDataCallbacks gDataCallbacks;

void ensureCrcTable() {
  if (gCrcTableReady)
    return;
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (uint32_t j = 0; j < 8; ++j) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc >>= 1;
      }
    }
    gCrcTable[i] = crc;
  }
  gCrcTableReady = true;
}

uint32_t crc32Update(uint32_t crc, const uint8_t *data, size_t len) {
  ensureCrcTable();
  uint32_t value = crc;
  for (size_t i = 0; i < len; ++i) {
    uint8_t index = (value ^ data[i]) & 0xFFu;
    value = (value >> 8) ^ gCrcTable[index];
  }
  return value;
}

uint32_t crc32Finalize(uint32_t crc) { return crc ^ 0xFFFFFFFFu; }

uint32_t crc32Compute(const uint8_t *data, size_t len) {
  return crc32Finalize(crc32Update(0xFFFFFFFFu, data, len));
}

void setStatusJson(const char *json) {
  if (!gStatusChar || !json)
    return;
  size_t len = strlen(json);
  gStatusChar->setValue((uint8_t *)json, len);
  gStatusChar->notify();
}

void publishIdle() { setStatusJson("{\"state\":\"idle\"}"); }

void publishError(const char *message, int32_t offset = -1,
                  int32_t received = -1) {
  if (!message)
    return;
  char json[kStatusBufferSize];
  if (offset >= 0 && received >= 0) {
    snprintf(json, sizeof(json),
             "{\"state\":\"error\",\"message\":\"%s\",\"offset\":%ld,\"received\":%ld}",
             message, static_cast<long>(offset), static_cast<long>(received));
  } else if (offset >= 0) {
    snprintf(json, sizeof(json),
             "{\"state\":\"error\",\"message\":\"%s\",\"offset\":%ld}", message,
             static_cast<long>(offset));
  } else if (received >= 0) {
    snprintf(json, sizeof(json),
             "{\"state\":\"error\",\"message\":\"%s\",\"received\":%ld}",
             message, static_cast<long>(received));
  } else {
    snprintf(json, sizeof(json),
             "{\"state\":\"error\",\"message\":\"%s\"}", message);
  }
  setStatusJson(json);
}

void publishReceiving(size_t received, size_t total) {
  char json[kStatusBufferSize];
  snprintf(
      json, sizeof(json),
      "{\"state\":\"receiving\",\"received\":%lu,\"total\":%lu}",
      static_cast<unsigned long>(received), static_cast<unsigned long>(total));
  setStatusJson(json);
}

void publishChunkAck(uint32_t nextOffset, size_t total) {
  char json[kStatusBufferSize];
  snprintf(json, sizeof(json),
           "{\"state\":\"chunk_ack\",\"next\":%lu,\"total\":%lu}",
           static_cast<unsigned long>(nextOffset),
           static_cast<unsigned long>(total));
  setStatusJson(json);
}

void publishValidating() { setStatusJson("{\"state\":\"validating\"}"); }

void publishReady() {
  setStatusJson("{\"state\":\"ready\",\"message\":\"rebooting\"}");
}

std::string trim(const std::string &input) {
  size_t start = 0;
  size_t end = input.size();
  while (start < end && std::isspace(static_cast<unsigned char>(input[start])))
    ++start;
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])))
    --end;
  return input.substr(start, end - start);
}

std::string toUpper(std::string value) {
  for (char &c : value) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return value;
}

bool parseKeyValuePayload(const std::string &payload,
                          std::map<std::string, std::string> &out) {
  out.clear();
  size_t start = 0;
  while (start < payload.size()) {
    size_t end = payload.find(';', start);
    std::string token =
        payload.substr(start, (end == std::string::npos) ? std::string::npos
                                                         : end - start);
    token = trim(token);
    if (!token.empty()) {
      size_t eq = token.find('=');
      if (eq != std::string::npos) {
        std::string key = toUpper(trim(token.substr(0, eq)));
        std::string value = trim(token.substr(eq + 1));
        if (!key.empty())
          out[key] = value;
      }
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return !out.empty();
}

bool parseSizeValue(const std::string &value, size_t &result) {
  if (value.empty())
    return false;
  char *end = nullptr;
  unsigned long long parsed = strtoull(value.c_str(), &end, 10);
  if (end == value.c_str() || *end != '\0')
    return false;
  result = static_cast<size_t>(parsed);
  return true;
}

int hexValue(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

bool parseSha256Hex(const std::string &value, uint8_t *out) {
  if (value.size() != 64 || !out)
    return false;
  for (size_t i = 0; i < 32; ++i) {
    int hi = hexValue(value[i * 2]);
    int lo = hexValue(value[i * 2 + 1]);
    if (hi < 0 || lo < 0)
      return false;
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

bool parseCrc32Hex(const std::string &value, uint32_t &crc) {
  if (value.empty() || value.size() > 8)
    return false;
  char *end = nullptr;
  unsigned long parsed = strtoul(value.c_str(), &end, 16);
  if (end == value.c_str() || *end != '\0')
    return false;
  crc = static_cast<uint32_t>(parsed);
  return true;
}

void handleAbortCommand() {
  if (!gSession.active) {
    publishError("no_session");
    return;
  }
  logPrintln("[ota] Client aborted OTA session");
  gSession.reset();
  publishError("aborted");
}

void sendOtaError(const char *message, int32_t offset = -1,
                  int32_t received = -1) {
  publishError(message, offset, received);
  if (gSession.active) {
    gSession.reset();
  }
}

bool verifyWrittenImage() {
  if (!gSession.partition)
    return false;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  if (mbedtls_sha256_starts_ret(&ctx, 0) != 0) {
    mbedtls_sha256_free(&ctx);
    return false;
  }
  uint32_t crc = 0xFFFFFFFFu;
  std::array<uint8_t, kValidationReadSize> buffer;
  size_t remaining = gSession.imageSize;
  size_t offset = 0;
  while (remaining > 0) {
    size_t toRead = std::min(remaining, buffer.size());
    esp_err_t err = esp_partition_read(gSession.partition, offset,
                                       buffer.data(), toRead);
    if (err != ESP_OK) {
      logPrintf("[ota] partition read failed: %d\n", static_cast<int>(err));
      mbedtls_sha256_free(&ctx);
      return false;
    }
    crc = crc32Update(crc, buffer.data(), toRead);
    if (mbedtls_sha256_update_ret(&ctx, buffer.data(), toRead) != 0) {
      mbedtls_sha256_free(&ctx);
      return false;
    }
    offset += toRead;
    remaining -= toRead;
  }
  uint32_t crcFinal = crc32Finalize(crc);
  uint8_t shaOut[32];
  if (mbedtls_sha256_finish_ret(&ctx, shaOut) != 0) {
    mbedtls_sha256_free(&ctx);
    return false;
  }
  mbedtls_sha256_free(&ctx);
  if (crcFinal != gSession.expectedCrc32) {
    logPrintf("[ota] CRC mismatch: expected %08lx got %08lx\n",
              static_cast<unsigned long>(gSession.expectedCrc32),
              static_cast<unsigned long>(crcFinal));
    return false;
  }
  if (memcmp(shaOut, gSession.expectedSha256, sizeof(shaOut)) != 0) {
    logPrintln("[ota] SHA mismatch after validation");
    return false;
  }
  return true;
}

void handleFinishCommand() {
  if (!gSession.active) {
    publishError("no_session");
    return;
  }
  if (gSession.received != gSession.imageSize) {
    publishError("size_mismatch", -1, gSession.received);
    gSession.reset();
    return;
  }

  publishValidating();

  esp_err_t err = esp_ota_end(gSession.handle);
  gSession.handle = 0;
  if (err != ESP_OK) {
    logPrintf("[ota] esp_ota_end failed: %d\n", static_cast<int>(err));
    sendOtaError("ota_end_failed");
    return;
  }

  if (!verifyWrittenImage()) {
    sendOtaError("verification_failed");
    return;
  }

  err = esp_ota_set_boot_partition(gSession.partition);
  if (err != ESP_OK) {
    logPrintf("[ota] esp_ota_set_boot_partition failed: %d\n",
              static_cast<int>(err));
    sendOtaError("boot_slot_error");
    return;
  }

  logPrintln("[ota] OTA image validated; reboot scheduled.");
  gSession.reset();
  publishReady();
  firmwareApp().requestRestart("ota_ready");
}

bool beginSession(size_t size, const uint8_t *sha256, uint32_t crc32) {
  gSession.reset();
  const esp_partition_t *partition =
      esp_ota_get_next_update_partition(nullptr);
  if (!partition) {
    publishError("no_partition");
    return false;
  }
  if (size == 0 || size > partition->size) {
    publishError("size_invalid");
    return false;
  }

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(partition, size, &handle);
  if (err != ESP_OK) {
    logPrintf("[ota] esp_ota_begin failed: %d\n", static_cast<int>(err));
    publishError("ota_begin_failed");
    return false;
  }

  gSession.active = true;
  gSession.imageSize = size;
  gSession.received = 0;
  gSession.lastProgressNotified = 0;
  gSession.nextOffset = 0;
  gSession.expectedCrc32 = crc32;
  memcpy(gSession.expectedSha256, sha256, 32);
  gSession.handle = handle;
  gSession.partition = partition;
  gSession.crcAccumulator = 0xFFFFFFFFu;
  gSession.shaCtxInitialized = true;
  mbedtls_sha256_init(&gSession.shaCtx);
  if (mbedtls_sha256_starts_ret(&gSession.shaCtx, 0) != 0) {
    gSession.reset();
    publishError("sha_init_failed");
    return false;
  }
  logPrintf("[ota] Session started for %lu bytes\n",
            static_cast<unsigned long>(size));
  publishReceiving(0, size);
  return true;
}

void handleStartCommand(const std::map<std::string, std::string> &kv) {
  if (gSession.active) {
    publishError("busy");
    return;
  }
  auto sizeIt = kv.find("SIZE");
  auto shaIt = kv.find("SHA256");
  auto crcIt = kv.find("CRC32");
  if (sizeIt == kv.end() || shaIt == kv.end() || crcIt == kv.end()) {
    publishError("missing_fields");
    return;
  }

  size_t size = 0;
  if (!parseSizeValue(sizeIt->second, size)) {
    publishError("invalid_size");
    return;
  }
  uint8_t sha[32];
  if (!parseSha256Hex(shaIt->second, sha)) {
    publishError("invalid_sha");
    return;
  }
  uint32_t crc = 0;
  if (!parseCrc32Hex(crcIt->second, crc)) {
    publishError("invalid_crc");
    return;
  }
  beginSession(size, sha, crc);
}

void parseAndHandleControlWrite(const std::string &value) {
  std::map<std::string, std::string> kv;
  if (!parseKeyValuePayload(value, kv)) {
    publishError("invalid_payload");
    return;
  }
  auto cmdIt = kv.find("CMD");
  if (cmdIt == kv.end()) {
    publishError("missing_cmd");
    return;
  }
  std::string cmd = toUpper(trim(cmdIt->second));
  if (cmd == "START") {
    handleStartCommand(kv);
  } else if (cmd == "FINISH") {
    handleFinishCommand();
  } else if (cmd == "ABORT") {
    handleAbortCommand();
  } else {
    publishError("unknown_cmd");
  }
}

void handleDataChunk(const std::string &value) {
  if (!gSession.active) {
    publishError("no_session");
    return;
  }
  const uint8_t *raw =
      reinterpret_cast<const uint8_t *>(value.data());
  size_t len = value.size();
  if (len < 10) {
    sendOtaError("chunk_format");
    return;
  }
  uint32_t offset = raw[0] | (raw[1] << 8) | (raw[2] << 16) | (raw[3] << 24);
  uint16_t payloadLen = raw[4] | (raw[5] << 8);
  size_t expectedSize = 4 + 2 + payloadLen + 4;
  if (payloadLen == 0 || payloadLen > kMaxChunkPayload || len != expectedSize) {
    sendOtaError("chunk_bounds", offset);
    return;
  }
  if (offset != gSession.nextOffset) {
    sendOtaError("offset_mismatch", offset);
    return;
  }
  if (gSession.received + payloadLen > gSession.imageSize) {
    sendOtaError("size_overflow", offset);
    return;
  }
  const uint8_t *payload = raw + 6;
  uint32_t chunkCrc =
      raw[6 + payloadLen] | (raw[7 + payloadLen] << 8) |
      (raw[8 + payloadLen] << 16) | (raw[9 + payloadLen] << 24);
  uint32_t computedCrc = crc32Compute(payload, payloadLen);
  if (chunkCrc != computedCrc) {
    sendOtaError("crc_mismatch", offset);
    return;
  }
  esp_err_t err =
      esp_ota_write(gSession.handle, payload, payloadLen);
  if (err != ESP_OK) {
    logPrintf("[ota] esp_ota_write failed: %d\n", static_cast<int>(err));
    sendOtaError("ota_write", offset);
    return;
  }
  gSession.received += payloadLen;
  gSession.nextOffset += payloadLen;
  gSession.crcAccumulator =
      crc32Update(gSession.crcAccumulator, payload, payloadLen);
  if (mbedtls_sha256_update_ret(&gSession.shaCtx, payload, payloadLen) != 0) {
    sendOtaError("sha_update_failed", offset);
    return;
  }

  if (gSession.received - gSession.lastProgressNotified >=
          kProgressIntervalBytes ||
      gSession.received == gSession.imageSize) {
    gSession.lastProgressNotified = gSession.received;
    publishReceiving(gSession.received, gSession.imageSize);
  }
  publishChunkAck(gSession.nextOffset, gSession.imageSize);
}

void OtaControlCallbacks::onWrite(NimBLECharacteristic *characteristic) {
  parseAndHandleControlWrite(characteristic->getValue());
}

void OtaDataCallbacks::onWrite(NimBLECharacteristic *characteristic) {
  handleDataChunk(characteristic->getValue());
}

} // namespace

void initOtaService(NimBLEServer *server) {
  if (!server)
    return;
  NimBLEService *service = server->createService(OTA_SERVICE_UUID);
  if (!service) {
    logPrintln("[ota] Failed to create OTA service");
    return;
  }
  gControlChar = service->createCharacteristic(
      OTA_CONTROL_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE |
          NIMBLE_PROPERTY::WRITE_NR);
  gDataChar =
      service->createCharacteristic(OTA_DATA_CHAR_UUID, NIMBLE_PROPERTY::WRITE);
  gStatusChar = service->createCharacteristic(
      OTA_STATUS_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

  gControlChar->setCallbacks(&gControlCallbacks);
  gDataChar->setCallbacks(&gDataCallbacks);
  publishIdle();
  service->start();
}

void otaHandleBleDisconnect() {
  if (!gSession.active)
    return;
  logPrintln("[ota] Disconnect detected, aborting OTA session");
  sendOtaError("disconnect");
}
