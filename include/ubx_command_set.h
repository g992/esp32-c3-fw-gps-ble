#ifndef UBX_COMMAND_SET_H
#define UBX_COMMAND_SET_H

#include <stddef.h>
#include <stdint.h>

struct UbxBinaryCommand {
  const uint8_t *data;
  size_t size;
};

struct UbxCommandSequence {
  const UbxBinaryCommand *commands;
  size_t length;
};

struct UbxKeyValue {
  uint32_t key;
  uint8_t value;
};

enum class UbxConfigProfile : uint8_t {
  FullSystems = 0,
  GlonassBeiDouGalileo = 1,
  GlonassOnly = 2,
  Custom = 3,
};

enum class UbxSettingsProfile : uint8_t {
  DefaultRamBbr = 0,
  CustomRam = 1,
};

constexpr size_t kUbxConfigProfileCount = 4;
constexpr size_t kUbxSettingsProfileCount = 2;
constexpr size_t kMaxUbxCustomCommandSize = 256;

extern const UbxBinaryCommand kUbxPingCommand;
extern const UbxCommandSequence kUbxDisableNmeaSequence;
extern const UbxCommandSequence kUbxEnableNmeaSequence;
extern const UbxCommandSequence kUbxDefaultSettingsSequence;

const UbxCommandSequence &ubxSettingsSequence(UbxSettingsProfile profile);
bool setCustomUbxSettingsCommand(const uint8_t *data, size_t size);
bool hasCustomUbxSettingsCommand();
size_t copyCustomUbxSettingsCommand(uint8_t *buffer, size_t capacity);

bool setCustomUbxProfileCommand(const uint8_t *data, size_t size);
bool hasCustomUbxProfileCommand();
size_t copyCustomUbxProfileCommand(uint8_t *buffer, size_t capacity);

const char *ubxSettingsProfileName(UbxSettingsProfile profile);

const UbxCommandSequence &ubxProfileSequence(UbxConfigProfile profile);
const UbxKeyValue *ubxProfileValidationTargets(UbxConfigProfile profile,
                                               size_t &count);
const char *ubxProfileName(UbxConfigProfile profile);
char ubxProfileToChar(UbxConfigProfile profile);
bool ubxProfileFromChar(char value, UbxConfigProfile &profileOut);
char ubxSettingsProfileToChar(UbxSettingsProfile profile);
bool ubxSettingsProfileFromChar(char value,
                                UbxSettingsProfile &profileOut);

#endif
