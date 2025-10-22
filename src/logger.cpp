#include "logger.h"

#include <cstdarg>

#include "system_mode.h"

namespace {
constexpr size_t kLogBufferSize = 192;
}

void logPrintln(const char *message) {
  if (!systemLogsEnabled() || !message) {
    return;
  }
  Serial.println(message);
}

void logPrintln(const __FlashStringHelper *message) {
  if (!systemLogsEnabled() || !message) {
    return;
  }
  Serial.println(message);
}

void logPrintf(const char *format, ...) {
  if (!systemLogsEnabled() || !format) {
    return;
  }
  char buffer[kLogBufferSize];
  va_list args;
  va_start(args, format);
  int len = vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (len <= 0) {
    return;
  }
  if (len < static_cast<int>(sizeof(buffer))) {
    Serial.print(buffer);
    return;
  }
  Serial.print(buffer);
  if (len >= static_cast<int>(sizeof(buffer))) {
    Serial.print(F("..."));
  }
}
