#include "ubx_command_set.h"

#include <string.h>

namespace {
constexpr uint8_t kMonVerRequest[] = {0xB5, 0x62, 0x0A, 0x04, 0x00,
                                      0x00, 0x0E, 0x34};

constexpr uint8_t kDisableNmeaCmd[] = {
    0xB5, 0x62, 0x06, 0x8A, 0x0E, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x02, 0x00, 0x73, 0x10, 0x00, 0x02, 0x00, 0x74,
    0x10, 0x00, 0xAA, 0x25};

constexpr uint8_t kEnableNmeaCmd[] = {
    0xB5, 0x62, 0x06, 0x8A, 0x0E, 0x00, 0x00, 0x01, 0x00,
    0x00, 0x02, 0x00, 0x74, 0x10, 0x01, 0x02, 0x00, 0x73,
    0x10, 0x01, 0xAC, 0x31};

constexpr uint8_t kDefaultRamCmd[] = {
    0xB5, 0x62, 0x06, 0x8A, 0x2F, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x06, 0x00, 0x36, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x21, 0x00, 0x11, 0x20, 0x04, 0x01, 0x00, 0x23,
    0x10, 0x01, 0x0D, 0x00, 0x41, 0x10, 0x01, 0x01, 0x00, 0x41,
    0x20, 0x08, 0x02, 0x00, 0x41, 0x20, 0x08, 0x01, 0x00, 0x21,
    0x30, 0x96, 0x00, 0xF3, 0xCB};

constexpr uint8_t kDefaultBbrCmd[] = {
    0xB5, 0x62, 0x06, 0x8A, 0x2F, 0x00, 0x00, 0x02, 0x00, 0x00,
    0x06, 0x00, 0x36, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x21, 0x00, 0x11, 0x20, 0x04, 0x01, 0x00, 0x23,
    0x10, 0x01, 0x0D, 0x00, 0x41, 0x10, 0x01, 0x01, 0x00, 0x41,
    0x20, 0x08, 0x02, 0x00, 0x41, 0x20, 0x08, 0x01, 0x00, 0x21,
    0x30, 0x96, 0x00, 0xF4, 0xF9};

constexpr uint8_t kGnssFullSystemsCmd[] = {
    0xB5, 0x62, 0x06, 0x8A, 0x4A, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x31, 0x10, 0x01, 0x05, 0x00, 0x31, 0x10, 0x01,
    0x07, 0x00, 0x31, 0x10, 0x01, 0x0D, 0x00, 0x31, 0x10, 0x00,
    0x0F, 0x00, 0x31, 0x10, 0x01, 0x12, 0x00, 0x31, 0x10, 0x01,
    0x14, 0x00, 0x31, 0x10, 0x01, 0x18, 0x00, 0x31, 0x10, 0x01,
    0x1F, 0x00, 0x31, 0x10, 0x01, 0x20, 0x00, 0x31, 0x10, 0x01,
    0x21, 0x00, 0x31, 0x10, 0x01, 0x22, 0x00, 0x31, 0x10, 0x01,
    0x24, 0x00, 0x31, 0x10, 0x01, 0x25, 0x00, 0x31, 0x10, 0x01,
    0xA9, 0xC3};

constexpr uint8_t kGnssGlonassBeiDouGalileoCmd[] = {
    0xB5, 0x62, 0x06, 0x8A, 0x4A, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x31, 0x10, 0x00, 0x05, 0x00, 0x31, 0x10, 0x01,
    0x07, 0x00, 0x31, 0x10, 0x01, 0x0D, 0x00, 0x31, 0x10, 0x00,
    0x0F, 0x00, 0x31, 0x10, 0x01, 0x12, 0x00, 0x31, 0x10, 0x00,
    0x14, 0x00, 0x31, 0x10, 0x01, 0x18, 0x00, 0x31, 0x10, 0x01,
    0x1F, 0x00, 0x31, 0x10, 0x00, 0x20, 0x00, 0x31, 0x10, 0x01,
    0x21, 0x00, 0x31, 0x10, 0x01, 0x22, 0x00, 0x31, 0x10, 0x01,
    0x24, 0x00, 0x31, 0x10, 0x00, 0x25, 0x00, 0x31, 0x10, 0x01,
    0xA5, 0x38};

constexpr uint8_t kGnssGlonassOnlyCmd[] = {
    0xB5, 0x62, 0x06, 0x8A, 0x4A, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x01, 0x00, 0x31, 0x10, 0x00, 0x05, 0x00, 0x31, 0x10, 0x00,
    0x07, 0x00, 0x31, 0x10, 0x00, 0x0D, 0x00, 0x31, 0x10, 0x00,
    0x0F, 0x00, 0x31, 0x10, 0x00, 0x12, 0x00, 0x31, 0x10, 0x00,
    0x14, 0x00, 0x31, 0x10, 0x01, 0x18, 0x00, 0x31, 0x10, 0x01,
    0x1F, 0x00, 0x31, 0x10, 0x00, 0x20, 0x00, 0x31, 0x10, 0x00,
    0x21, 0x00, 0x31, 0x10, 0x00, 0x22, 0x00, 0x31, 0x10, 0x00,
    0x24, 0x00, 0x31, 0x10, 0x00, 0x25, 0x00, 0x31, 0x10, 0x01,
    0x9F, 0x65};

const UbxBinaryCommand kDisableNmeaCommands[] = {
    {kDisableNmeaCmd, sizeof(kDisableNmeaCmd)}};

const UbxBinaryCommand kEnableNmeaCommands[] = {
    {kEnableNmeaCmd, sizeof(kEnableNmeaCmd)}};

const UbxBinaryCommand kDefaultSettingCommands[] = {
    {kDefaultRamCmd, sizeof(kDefaultRamCmd)},
    {kDefaultBbrCmd, sizeof(kDefaultBbrCmd)}};

uint8_t gCustomSettingsBuffer[kMaxUbxCustomCommandSize] = {};
uint8_t gCustomProfileBuffer[kMaxUbxCustomCommandSize] = {};
UbxBinaryCommand gCustomSettingsCommand = {gCustomSettingsBuffer, 0};
UbxBinaryCommand gCustomProfileCommand = {gCustomProfileBuffer, 0};
UbxCommandSequence gCustomSettingsSequence = {&gCustomSettingsCommand, 0};
UbxCommandSequence gCustomProfileSequence = {&gCustomProfileCommand, 0};

const UbxBinaryCommand kFullSystemsCommands[] = {
    {kGnssFullSystemsCmd, sizeof(kGnssFullSystemsCmd)}};

const UbxBinaryCommand kGlonassBeiDouGalileoCommands[] = {
    {kGnssGlonassBeiDouGalileoCmd, sizeof(kGnssGlonassBeiDouGalileoCmd)}};

const UbxBinaryCommand kGlonassOnlyCommands[] = {
    {kGnssGlonassOnlyCmd, sizeof(kGnssGlonassOnlyCmd)}};

constexpr UbxKeyValue kGlonassBeiDouGalileoValidation[] = {
    {0x10310001u, 0}, {0x10310005u, 1}, {0x10310007u, 1}, {0x1031000Du, 0},
    {0x1031000Fu, 1}, {0x10310012u, 0}, {0x10310014u, 1}, {0x10310018u, 1},
    {0x1031001Fu, 0}, {0x10310020u, 1}, {0x10310021u, 1}, {0x10310022u, 1},
    {0x10310024u, 0}, {0x10310025u, 1}};

constexpr UbxKeyValue kGlonassOnlyValidation[] = {
    {0x10310001u, 0}, {0x10310005u, 0}, {0x10310007u, 0}, {0x1031000Du, 0},
    {0x1031000Fu, 0}, {0x10310012u, 0}, {0x10310014u, 1}, {0x10310018u, 1},
    {0x1031001Fu, 0}, {0x10310020u, 0}, {0x10310021u, 0}, {0x10310022u, 0},
    {0x10310024u, 0}, {0x10310025u, 1}};

constexpr UbxKeyValue kFullProfileValidation[] = {
    {0x10310001u, 1}, {0x10310005u, 1}, {0x10310007u, 1}, {0x1031000Du, 0},
    {0x1031000Fu, 1}, {0x10310012u, 1}, {0x10310014u, 1}, {0x10310018u, 1},
    {0x1031001Fu, 1}, {0x10310020u, 1}, {0x10310021u, 1}, {0x10310022u, 1},
    {0x10310024u, 1}, {0x10310025u, 1}};

const UbxCommandSequence kFullProfileSequence = {kFullSystemsCommands,
                                                 sizeof(kFullSystemsCommands) /
                                                     sizeof(kFullSystemsCommands[0])};
const UbxCommandSequence kGlonassBeiDouGalileoSequence = {
    kGlonassBeiDouGalileoCommands,
    sizeof(kGlonassBeiDouGalileoCommands) /
        sizeof(kGlonassBeiDouGalileoCommands[0])};
const UbxCommandSequence kGlonassOnlySequence = {kGlonassOnlyCommands,
                                                 sizeof(kGlonassOnlyCommands) /
                                                     sizeof(kGlonassOnlyCommands[0])};

struct UbxProfileDescriptor {
  const char *name;
  const UbxCommandSequence *sequence;
  const UbxKeyValue *validation;
  size_t validationCount;
};

constexpr UbxProfileDescriptor kProfileTable[] = {
    {"Full systems", &kFullProfileSequence, kFullProfileValidation,
     sizeof(kFullProfileValidation) / sizeof(kFullProfileValidation[0])},
    {"GLONASS+BeiDou+Galileo", &kGlonassBeiDouGalileoSequence,
     kGlonassBeiDouGalileoValidation,
     sizeof(kGlonassBeiDouGalileoValidation) /
         sizeof(kGlonassBeiDouGalileoValidation[0])},
    {"GLONASS only", &kGlonassOnlySequence, kGlonassOnlyValidation,
     sizeof(kGlonassOnlyValidation) / sizeof(kGlonassOnlyValidation[0])}};

constexpr size_t kUbxBuiltinProfileCount =
    sizeof(kProfileTable) / sizeof(kProfileTable[0]);

static_assert(kUbxBuiltinProfileCount == 3,
              "Unexpected UBX profile table size");

bool hasValidCustomCommand(const UbxCommandSequence &sequence,
                           const UbxBinaryCommand &command) {
  return sequence.length > 0 && command.data && command.size >= 8;
}

bool storeCustomCommand(const uint8_t *data, size_t size,
                        UbxBinaryCommand &command,
                        UbxCommandSequence &sequence,
                        uint8_t *buffer) {
  if (!data || size == 0 || size > kMaxUbxCustomCommandSize)
    return false;
  memcpy(buffer, data, size);
  command.data = buffer;
  command.size = size;
  sequence.length = 1;
  return true;
}

size_t copyCustomCommand(const UbxBinaryCommand &command, uint8_t *buffer,
                         size_t capacity) {
  if (!buffer || capacity == 0)
    return 0;
  if (!command.data || command.size == 0 || command.size > capacity)
    return 0;
  memcpy(buffer, command.data, command.size);
  return command.size;
}
} // namespace

const UbxBinaryCommand kUbxPingCommand = {kMonVerRequest,
                                          sizeof(kMonVerRequest)};

const UbxCommandSequence kUbxDisableNmeaSequence = {
    kDisableNmeaCommands,
    sizeof(kDisableNmeaCommands) / sizeof(kDisableNmeaCommands[0])};

const UbxCommandSequence kUbxEnableNmeaSequence = {
    kEnableNmeaCommands,
    sizeof(kEnableNmeaCommands) / sizeof(kEnableNmeaCommands[0])};

const UbxCommandSequence kUbxDefaultSettingsSequence = {
    kDefaultSettingCommands,
    sizeof(kDefaultSettingCommands) / sizeof(kDefaultSettingCommands[0])};

const UbxCommandSequence &ubxSettingsSequence(
    UbxSettingsProfile profile) {
  if (profile == UbxSettingsProfile::CustomRam &&
      hasValidCustomCommand(gCustomSettingsSequence,
                            gCustomSettingsCommand)) {
    return gCustomSettingsSequence;
  }
  return kUbxDefaultSettingsSequence;
}

bool setCustomUbxSettingsCommand(const uint8_t *data, size_t size) {
  return storeCustomCommand(data, size, gCustomSettingsCommand,
                            gCustomSettingsSequence,
                            gCustomSettingsBuffer);
}

bool hasCustomUbxSettingsCommand() {
  return hasValidCustomCommand(gCustomSettingsSequence,
                               gCustomSettingsCommand);
}

size_t copyCustomUbxSettingsCommand(uint8_t *buffer, size_t capacity) {
  return copyCustomCommand(gCustomSettingsCommand, buffer, capacity);
}

bool setCustomUbxProfileCommand(const uint8_t *data, size_t size) {
  return storeCustomCommand(data, size, gCustomProfileCommand,
                            gCustomProfileSequence, gCustomProfileBuffer);
}

bool hasCustomUbxProfileCommand() {
  return hasValidCustomCommand(gCustomProfileSequence,
                               gCustomProfileCommand);
}

size_t copyCustomUbxProfileCommand(uint8_t *buffer, size_t capacity) {
  return copyCustomCommand(gCustomProfileCommand, buffer, capacity);
}

const UbxCommandSequence &ubxProfileSequence(UbxConfigProfile profile) {
  if (profile == UbxConfigProfile::Custom &&
      hasValidCustomCommand(gCustomProfileSequence,
                            gCustomProfileCommand)) {
    return gCustomProfileSequence;
  }
  size_t index = static_cast<size_t>(profile);
  if (index >= kUbxBuiltinProfileCount) {
    index = 0;
  }
  return *kProfileTable[index].sequence;
}

const UbxKeyValue *ubxProfileValidationTargets(UbxConfigProfile profile,
                                               size_t &count) {
  if (profile == UbxConfigProfile::Custom) {
    count = 0;
    return nullptr;
  }
  size_t index = static_cast<size_t>(profile);
  if (index >= kUbxBuiltinProfileCount) {
    index = 0;
  }
  count = kProfileTable[index].validationCount;
  return kProfileTable[index].validation;
}

const char *ubxProfileName(UbxConfigProfile profile) {
  if (profile == UbxConfigProfile::Custom) {
    return "Custom";
  }
  size_t index = static_cast<size_t>(profile);
  if (index >= kUbxBuiltinProfileCount) {
    index = 0;
  }
  return kProfileTable[index].name;
}

const char *ubxSettingsProfileName(UbxSettingsProfile profile) {
  switch (profile) {
  case UbxSettingsProfile::DefaultRamBbr:
    return "Default RAM+BBR";
  case UbxSettingsProfile::CustomRam:
    return "Custom RAM";
  }
  return "Unknown";
}

char ubxProfileToChar(UbxConfigProfile profile) {
  size_t index = static_cast<size_t>(profile);
  if (index >= 10) {
    index = 0;
  }
  return static_cast<char>('0' + index);
}

bool ubxProfileFromChar(char value, UbxConfigProfile &profileOut) {
  if (value < '0' || value >= static_cast<char>('0' + kUbxConfigProfileCount)) {
    return false;
  }
  profileOut = static_cast<UbxConfigProfile>(value - '0');
  return true;
}

char ubxSettingsProfileToChar(UbxSettingsProfile profile) {
  size_t index = static_cast<size_t>(profile);
  if (index >= 10) {
    index = 0;
  }
  return static_cast<char>('0' + index);
}

bool ubxSettingsProfileFromChar(char value,
                                UbxSettingsProfile &profileOut) {
  if (value < '0' ||
      value >= static_cast<char>('0' + kUbxSettingsProfileCount)) {
    return false;
  }
  profileOut = static_cast<UbxSettingsProfile>(value - '0');
  return true;
}
