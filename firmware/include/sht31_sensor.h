#pragma once

#include "config.h"

struct AmbientReading {
  float temperatureC = NAN;
  float humidityPct = NAN;
  bool valid = false;
};

class Sht31Sensor {
 public:
  bool begin();
  AmbientReading read();
  bool ok() const { return ok_; }

 private:
  bool ok_ = false;
  uint8_t addr_ = SHT31_I2C_ADDR;
};
