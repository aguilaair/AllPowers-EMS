#pragma once

#include <Arduino.h>
#include <Wire.h>

// Goodix GT911 on Waveshare ESP32-S3-Touch-LCD-2.8C (I2C 0x5D, INT GPIO16, RST EXIO2).

struct TouchPoint {
  bool pressed = false;
  uint16_t x = 0;
  uint16_t y = 0;
};

class Gt911Touch {
 public:
  using ExioSetter = void (*)(uint8_t bit, bool level);

  bool begin(ExioSetter setExio);
  // Returns true on a new press edge (touch down).
  bool readPress(TouchPoint &out);

 private:
  bool reset();
  bool i2cRead(uint16_t reg, uint8_t *data, size_t len);
  bool i2cWrite(uint16_t reg, const uint8_t *data, size_t len);

  ExioSetter setExio_ = nullptr;
  bool wasPressed_ = false;
  uint32_t lastReadMs_ = 0;
};
