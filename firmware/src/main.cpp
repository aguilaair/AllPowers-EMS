#include <Arduino.h>
#include <Wire.h>
#include <esp_task_wdt.h>

#include "allpowers_ble.h"
#include "broadlink_rm.h"
#include "config.h"
#include "haier_ir.h"
#include "ems_controller.h"
#include "sht31_sensor.h"
#include "status_ui.h"

namespace {
AllPowersBle g_ble;
Sht31Sensor g_sht;
EmsController g_ems;
StatusUi g_ui;
BroadlinkRm g_bl;

uint32_t g_lastShtMs = 0;
bool g_lastBle = false;
bool g_lastBl = false;
bool g_learning = false;

void bootLine(const char *msg) { g_ui.showBootStatus(msg); }

void syncAcOutlet(bool on) {
  if (!g_ble.isConnected()) {
    return;
  }
  // Don't rewrite when pack already matches — avoids CA glitches on connect.
  if (g_ble.state().hasStatus && g_ble.state().acOpen == on) {
    return;
  }
  g_ble.setAc(on);
}

bool sendClimateIr(ClimateCmd cmd) {
  if (g_learning) {
    return false;
  }
  if (!g_bl.isReady()) {
    Serial.println(F("[MAIN] IR omitido — Broadlink no listo"));
    return false;
  }
  const uint8_t *blob = nullptr;
  size_t len = 0;
  switch (cmd) {
    case ClimateCmd::Cool:
      blob = kHaierCool26;
      len = kHaierCool26Len;
      break;
    case ClimateCmd::Heat:
      blob = kHaierHeat18;
      len = kHaierHeat18Len;
      break;
    case ClimateCmd::Off:
    default:
      blob = kHaierOff;
      len = kHaierOffLen;
      break;
  }
  Serial.printf("[MAIN] IR clima %s (set %d/%d°C learned)\n", climateCmdName(cmd),
                IR_COOL_SETPOINT_C, IR_HEAT_SETPOINT_C);
  return g_bl.sendIr(blob, len, true);
}

void dumpIrCArray(const char *symbol, const uint8_t *data, size_t len) {
  Serial.printf("// %s: %u bytes\n", symbol, static_cast<unsigned>(len));
  Serial.printf("static const uint8_t %s[%u] PROGMEM = {\n", symbol,
                static_cast<unsigned>(len));
  for (size_t i = 0; i < len; ++i) {
    if ((i % 12) == 0) {
      Serial.print(F("    "));
    }
    Serial.printf("0x%02X", data[i]);
    if (i + 1 < len) {
      Serial.print(F(","));
    }
    if ((i % 12) == 11 || i + 1 == len) {
      Serial.println();
    } else {
      Serial.print(F(" "));
    }
  }
  Serial.println(F("};"));
  Serial.printf("static constexpr size_t %sLen = %u;\n\n", symbol,
                static_cast<unsigned>(len));
}

bool learnOne(const char *label, const char *hint, const char *symbol, uint8_t *buf,
              size_t bufMax, size_t &outLen) {
  Serial.println();
  Serial.printf("[LEARN] %s\n", label);
  Serial.printf("[LEARN] %s\n", hint);
  Serial.println(F("[LEARN] apunta el Chunghop al Mini y pulsa (30s)…"));
  if (!g_bl.learnIrBlocking(buf, bufMax, outLen, 30000)) {
    Serial.printf("[LEARN] FALLÓ %s\n", label);
    return false;
  }
  if (outLen < 4 || buf[0] != 0x26) {
    Serial.printf("[LEARN] blob sospechoso (len=%u head=0x%02X)\n",
                  static_cast<unsigned>(outLen), outLen ? buf[0] : 0);
  }
  dumpIrCArray(symbol, buf, outLen);
  return true;
}

void runLearnSession() {
  if (!g_bl.isReady()) {
    Serial.println(F("[LEARN] Broadlink no listo — espera LED IR verde"));
    return;
  }
  g_learning = true;
  Serial.println();
  Serial.println(F("======== LEARN IR (Chunghop code 13) ========"));
  Serial.println(F("Captura Cool@26, Heat@18, Off. Pega en include/haier_ir.h"));
  Serial.println(F("============================================="));

  constexpr size_t kMaxIr = 640;
  size_t coolLen = 0, heatLen = 0, offLen = 0;
  uint8_t cool[kMaxIr], heat[kMaxIr], off[kMaxIr];

  const bool okCool =
      learnOne("Cool@26", "Modo COOL, temp 26, fan auto — envía (POWER/OK)",
               "kHaierCool26", cool, kMaxIr, coolLen);
  const bool okHeat =
      okCool && learnOne("Heat@18", "Modo HEAT, temp 18, fan auto — envía",
                         "kHaierHeat18", heat, kMaxIr, heatLen);
  const bool okOff =
      okHeat && learnOne("Off", "Apaga el AC (POWER off) — envía", "kHaierOff", off,
                         kMaxIr, offLen);

  Serial.println();
  if (okCool && okHeat && okOff) {
    Serial.println(F("======== PEGAR EN haier_ir.h ========"));
    Serial.println(F("#pragma once"));
    Serial.println(F("#include <Arduino.h>"));
    Serial.println();
    Serial.println(F("// Learned from Chunghop K-830ES code 13 (Haier/Yair) via Mini."));
    Serial.println(F("// Cool@26 / Heat@18 / Off — raw Broadlink IR (0x26…)."));
    Serial.println();
    dumpIrCArray("kHaierOff", off, offLen);
    dumpIrCArray("kHaierCool26", cool, coolLen);
    dumpIrCArray("kHaierHeat18", heat, heatLen);
    Serial.println(F("======== FIN — rebuild + flash ========"));
  } else {
    Serial.println(F("[LEARN] incompleto — escribe learn para reintentar"));
  }
  g_learning = false;
}

void pollSerialCommands() {
  static char line[48];
  static size_t len = 0;
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      line[len] = '\0';
      if (len > 0) {
        if (strcmp(line, "learn") == 0) {
          runLearnSession();
        } else {
          Serial.printf("[MAIN] cmd desconocido: %s (usa: learn)\n", line);
        }
      }
      len = 0;
      continue;
    }
    if (len + 1 < sizeof(line)) {
      line[len++] = c;
    } else {
      len = 0;
    }
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.beginTransmission(0x20);
  Wire.write(0x03);
  Wire.write(0x00);
  Wire.endTransmission();
  Wire.beginTransmission(0x20);
  Wire.write(0x01);
  Wire.write(0x7F);
  Wire.endTransmission();

  Serial.println();
  Serial.println(F("=== Refugio EMS (AllPowers) ==="));
  Serial.println(F("CC ON | CA: AC+luces (corte ≤15%) | clima IR"));

  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);

  g_ems.begin();
  g_ui.begin();
  bootLine("Sensor…");
  g_sht.begin();
  bootLine("Sensor OK");

  // BLE first — SoftAP-then-BT coexistence aborts on ESP32-S3.
  g_ble.onStatus([](const AllPowersProto::DeviceState &st) {
    g_ems.onBattery(st);
    if (!st.dcOpen) {
      Serial.println(F("[MAIN] CC reportada OFF — reactivando"));
      g_ble.ensureDcOn();
    }
    // Keep CA aligned with EMS (hard cut at ≤15% SOC).
    const bool want = g_ems.acOutletDesired();
    if (st.acOpen != want) {
      Serial.printf("[MAIN] CA reportada %s — corrigiendo a %s\n", st.acOpen ? "ON" : "OFF",
                    want ? "ON" : "OFF");
      g_ble.setAc(want);
    }
  });

  bootLine("Bateria BLE…");
  g_ble.begin();
  g_ems.onBleConnected(g_ble.isConnected());
  g_lastBle = g_ble.isConnected();
  if (g_ble.isConnected()) {
    // After connect we already merged DC from status; only nudge CA if needed
    // and we know the pack's current AC flag (avoids AC-off pulse).
    if (g_ble.state().hasStatus) {
      syncAcOutlet(g_ems.acOutletDesired());
    }
    bootLine("Bateria OK");
  } else {
    bootLine("Bateria —");
  }

  g_bl.onProgress(+[](const char *msg) { bootLine(msg); });
  if (!g_bl.begin()) {
    bootLine("IR — esperando");
    Serial.println(F("[MAIN] Broadlink diferido — SoftAP activo"));
  }
  g_ui.setBroadlinkOk(g_bl.isReady());
  g_lastBl = g_bl.isReady();
  g_bl.onProgress(nullptr);  // stop boot-line updates after splash

  bootLine(g_bl.isReady() && g_ble.isConnected() ? "Listo" : "Listo (parcial)");
  delay(600);

  // Push current climate command once IR is up (usually Off at boot).
  if (g_bl.isReady()) {
    sendClimateIr(g_ems.climateCommanded());
  }

  Serial.println(F("[MAIN] arranque completo"));
  Serial.println(F("[MAIN] serial: escribe 'learn' para capturar IR del mando"));
}

void loop() {
  esp_task_wdt_reset();

  pollSerialCommands();

  g_ble.loop();
  g_bl.loop();

  const bool bleNow = g_ble.isConnected();
  if (bleNow != g_lastBle) {
    g_ems.onBleConnected(bleNow);
    g_lastBle = bleNow;
    if (!bleNow) {
      Serial.println(F("[MAIN] BLE perdido — clima inhibido"));
    } else {
      // Wait for status notify — onStatus ensures DC and aligns CA without
      // writing AC=0 from defaults.
      Serial.println(F("[MAIN] BLE recuperado — esperando status"));
    }
  }

  const bool blNow = g_bl.isReady();
  if (blNow != g_lastBl) {
    g_lastBl = blNow;
    g_ui.setBroadlinkOk(blNow);
    if (blNow) {
      Serial.println(F("[MAIN] Broadlink online — reenviando clima"));
      sendClimateIr(g_ems.climateCommanded());
    } else {
      Serial.println(F("[MAIN] Broadlink offline"));
    }
  }

  const uint32_t now = millis();
  if (now - g_lastShtMs >= SHT31_READ_MS) {
    g_lastShtMs = now;
    g_ems.onAmbient(g_sht.read());
  }

  // Touch first so overrides set forceTick before EMS tick
  g_ui.poll(g_ems);

  ClimateCmd cmd = ClimateCmd::Off;
  bool outletChanged = false;
  if (g_ems.tick(cmd, outletChanged)) {
    // IR Off before CA cut when both change in the same tick.
    sendClimateIr(cmd);
  }
  if (outletChanged) {
    syncAcOutlet(g_ems.acOutletDesired());
  }

  g_ui.draw(g_ems);

  delay(20);
}
