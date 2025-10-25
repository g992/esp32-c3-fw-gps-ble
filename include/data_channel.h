#ifndef DATA_CHANNEL_H
#define DATA_CHANNEL_H

#include <Arduino.h>
#include <stdint.h>

struct NavDataSample {
  float latitude = 0.0f;
  float longitude = 0.0f;
  float heading = 0.0f;
  float speed = 0.0f;
  float altitude = 0.0f;
};

struct SystemStatusSample {
  uint8_t fix = 0;
  float hdop = 0.0f;
  uint8_t satellites = 0;
  int32_t ttffSeconds = -1;
  String signalsJson;
};

class NavDataPublisher {
public:
  virtual ~NavDataPublisher() = default;
  virtual void publishNavData(const NavDataSample &sample) = 0;
};

class SystemStatusPublisher {
public:
  virtual ~SystemStatusPublisher() = default;
  virtual void publishSystemStatus(const SystemStatusSample &sample) = 0;
};

#endif
