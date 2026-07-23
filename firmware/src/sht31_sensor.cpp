#include "sht31_sensor.h"

#include <Adafruit_SHT31.h>
#include <Wire.h>

namespace {
Adafruit_SHT31 g_sht;
}

bool Sht31Sensor::begin() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(100000);

  addr_ = SHT31_I2C_ADDR;
  if (!g_sht.begin(addr_)) {
    Serial.printf("[SHT31] 0x%02X failed, trying 0x45\n", addr_);
    addr_ = 0x45;
    if (!g_sht.begin(addr_)) {
      Serial.println(F("[SHT31] not found"));
      ok_ = false;
      return false;
    }
  }

  ok_ = true;
  Serial.printf("[SHT31] ok at 0x%02X\n", addr_);
  return true;
}

AmbientReading Sht31Sensor::read() {
  AmbientReading r;
  if (!ok_) {
    return r;
  }

  float t = g_sht.readTemperature();
  float h = g_sht.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println(F("[SHT31] read failed"));
    r.valid = false;
    return r;
  }

  r.temperatureC = t;
  r.humidityPct = h;
  r.valid = true;
  return r;
}
