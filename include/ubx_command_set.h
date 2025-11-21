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
};

constexpr size_t kUbxConfigProfileCount = 3;

extern const UbxBinaryCommand kUbxPingCommand;
extern const UbxCommandSequence kUbxDisableNmeaSequence;
extern const UbxCommandSequence kUbxEnableNmeaSequence;
extern const UbxCommandSequence kUbxDefaultSettingsSequence;

const UbxCommandSequence &ubxProfileSequence(UbxConfigProfile profile);
const UbxKeyValue *ubxProfileValidationTargets(UbxConfigProfile profile,
                                               size_t &count);
const char *ubxProfileName(UbxConfigProfile profile);
char ubxProfileToChar(UbxConfigProfile profile);
bool ubxProfileFromChar(char value, UbxConfigProfile &profileOut);

#endif
