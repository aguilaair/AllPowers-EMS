#include "gt911_touch.h"

namespace {
constexpr uint8_t GT911_ADDR = 0x5D;
constexpr int GT911_INT_PIN = 16;
constexpr uint8_t EXIO_TP_RST = 1;  // EXIO2 → bit 1
constexpr uint16_t REG_STATUS = 0x814E;
constexpr uint16_t REG_POINTS = 0x814F;
}  // namespace

bool Gt911Touch::i2cRead(uint16_t reg, uint8_t *data, size_t len) {
  Wire.beginTransmission(GT911_ADDR);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  const size_t got = Wire.requestFrom(GT911_ADDR, static_cast<uint8_t>(len));
  if (got != len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    data[i] = Wire.read();
  }
  return true;
}

bool Gt911Touch::i2cWrite(uint16_t reg, const uint8_t *data, size_t len) {
  Wire.beginTransmission(GT911_ADDR);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  for (size_t i = 0; i < len; ++i) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission() == 0;
}

bool Gt911Touch::reset() {
  if (!setExio_) {
    return false;
  }
  pinMode(GT911_INT_PIN, OUTPUT);
  digitalWrite(GT911_INT_PIN, LOW);
  setExio_(EXIO_TP_RST, false);
  delay(10);
  setExio_(EXIO_TP_RST, true);
  delay(50);
  pinMode(GT911_INT_PIN, INPUT);
  delay(50);
  return true;
}

bool Gt911Touch::begin(ExioSetter setExio) {
  setExio_ = setExio;
  if (!reset()) {
    return false;
  }
  uint8_t id[3] = {0};
  if (!i2cRead(0x8140, id, 3)) {
    Serial.println(F("[TOUCH] GT911 no responde"));
    return false;
  }
  Serial.printf("[TOUCH] GT911 id=%02X %02X %02X\n", id[0], id[1], id[2]);
  return true;
}

bool Gt911Touch::readPress(TouchPoint &out) {
  out = {};
  const uint32_t now = millis();
  if (now - lastReadMs_ < 30) {
    return false;
  }
  lastReadMs_ = now;

  uint8_t status = 0;
  if (!i2cRead(REG_STATUS, &status, 1)) {
    return false;
  }

  const bool ready = (status & 0x80) != 0;
  const uint8_t points = status & 0x0F;
  uint8_t clear = 0;
  i2cWrite(REG_STATUS, &clear, 1);

  bool pressed = false;
  uint16_t x = 0;
  uint16_t y = 0;
  if (ready && points > 0 && points <= 5) {
    uint8_t buf[8] = {0};
    if (i2cRead(REG_POINTS, buf, 8)) {
      // track, xl, xh, yl, yh, …
      x = static_cast<uint16_t>((buf[2] << 8) | buf[1]);
      y = static_cast<uint16_t>((buf[4] << 8) | buf[3]);
      if (x < 480 && y < 480) {
        pressed = true;
      }
    }
  }

  const bool edge = pressed && !wasPressed_;
  wasPressed_ = pressed;
  if (edge) {
    out.pressed = true;
    out.x = x;
    out.y = y;
    return true;
  }
  return false;
}
