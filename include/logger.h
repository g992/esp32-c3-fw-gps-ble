#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

void logPrintln(const char *message);
void logPrintln(const __FlashStringHelper *message);
void logPrintf(const char *format, ...);

#endif
