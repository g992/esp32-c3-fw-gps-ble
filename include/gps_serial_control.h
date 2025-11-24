#ifndef GPS_SERIAL_CONTROL_H
#define GPS_SERIAL_CONTROL_H

#include <stdint.h>
#include <string>

#include "ubx_command_set.h"

constexpr uint32_t GPS_BAUD_MIN = 4800;
constexpr uint32_t GPS_BAUD_MAX = 921600;

uint32_t getGpsSerialBaud();
bool setGpsSerialBaud(uint32_t baud);
UbxConfigProfile getGpsUbxProfile();
bool setGpsUbxProfile(UbxConfigProfile profile);
UbxSettingsProfile getGpsUbxSettingsProfile();
bool setGpsUbxSettingsProfile(UbxSettingsProfile profile);
bool setGpsCustomProfileCommand(const std::string &hex);
bool setGpsCustomSettingsCommand(const std::string &hex);
std::string getGpsCustomProfileCommand();
std::string getGpsCustomSettingsCommand();

#endif
