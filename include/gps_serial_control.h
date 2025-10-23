#ifndef GPS_SERIAL_CONTROL_H
#define GPS_SERIAL_CONTROL_H

#include <stdint.h>

constexpr uint32_t GPS_BAUD_MIN = 4800;
constexpr uint32_t GPS_BAUD_MAX = 921600;

uint32_t getGpsSerialBaud();
bool setGpsSerialBaud(uint32_t baud);

#endif
