#pragma once

#include <Arduino.h>

// ---- Board I2C (Waveshare ESP32-S3-Touch-LCD-2.8C) ----
static constexpr int PIN_I2C_SDA = 15;
static constexpr int PIN_I2C_SCL = 7;
static constexpr uint8_t SHT31_I2C_ADDR = 0x44;  // ADDR pin low; use 0x45 if conflict
static constexpr int PIN_LCD_BL = 6;

// ---- AllPowers BLE ----
static constexpr const char *BLE_DEVICE_NAME_PREFIX = "AP R4000";
// Optional fixed MAC (empty = scan by name). Example: "AA:BB:CC:DD:EE:FF"
static constexpr const char *BLE_DEVICE_MAC = "";

// ---- SOC gates ----
// Start only when the pack is healthy (avoids living at mid/low SOC).
// Extreme ambient may start earlier; once running, ride down to HOLD.
static constexpr int SOC_ENABLE_COOL_PCT = 60;     // normal cool start
// Extreme ambient: allow an earlier *start* for caged/recovering cats.
static constexpr int SOC_EMERGENCY_COOL_PCT = 40;  // cool start if ≥ COOL_EMERGENCY_C
static constexpr int SOC_HOLD_COOL_PCT = 35;       // keep cooling until below this
static constexpr int SOC_ENABLE_HEAT_PCT = 50;     // normal heat start
static constexpr int SOC_EMERGENCY_HEAT_PCT = 35;  // heat start if ≤ HEAT_EMERGENCY_C
static constexpr int SOC_HOLD_HEAT_PCT = 30;       // keep heating until below this
static constexpr float COOL_EMERGENCY_C = 32.0f;  // heat stress for confined cats
static constexpr float HEAT_EMERGENCY_C = 10.0f;  // cold stress for confined cats
static constexpr int SOC_RESERVE_CUT_PCT = 20;  // stop IR climate (hard)
static constexpr int SOC_RESERVE_CLEAR_PCT = 25;
static constexpr int SOC_AC_OUTLET_CUT_PCT = 15;  // hard-kill CA (stuck IR)
static constexpr int SOC_AC_OUTLET_CLEAR_PCT = 18;

// ---- Temperature (°C) — cat-safe, ~2°C AC overshoot baked into OFF ----
// Cool: start before heat stress; OFF when room has undershot setpoint.
static constexpr float COOL_ON_C = 28.0f;
static constexpr float COOL_OFF_C = 24.0f;
// Heat: avoid cold caseta floors; OFF after mild overshoot above setpoint.
static constexpr float HEAT_ON_C = 12.0f;
static constexpr float HEAT_OFF_C = 16.0f;

// IR setpoints (learned Chunghop K-830ES code 13 / Haier-Yair).
static constexpr int IR_COOL_SETPOINT_C = 26;
static constexpr int IR_HEAT_SETPOINT_C = 18;

// Compressor anti-short-cycle (battery reserve cut bypasses min ON)
static constexpr uint32_t MIN_AC_ON_MS = 15UL * 60UL * 1000UL;   // 15 min
static constexpr uint32_t MIN_AC_OFF_MS = 10UL * 60UL * 1000UL;  // 10 min

// Loop timing
static constexpr uint32_t EMS_TICK_MS = 5000;
static constexpr uint32_t SHT31_READ_MS = 2000;
// UI redraws only on dirty/state change (no fixed refresh timer).
static constexpr uint32_t BLE_RECONNECT_BASE_MS = 2000;
// Connected but no valid status notify → resubscribe / reconnect.
static constexpr uint32_t BLE_STATUS_STALE_MS = 8000;
static constexpr uint32_t WATCHDOG_TIMEOUT_S = 30;

// LCD backlight PWM (0–255); keep moderate — board runs from DC rail
static constexpr uint8_t LCD_BACKLIGHT_DUTY = 140;

// ---- Broadlink RM Mini 3 (local IR) ----
// ESP SoftAP that the Mini joins after provisioning from BroadlinkProv.
static constexpr const char *EMS_WIFI_SSID = "Refugio-EMS";
static constexpr const char *EMS_WIFI_PASS = "refugioems";  // WPA2, ≥8 chars
static constexpr const char *BROADLINK_AP_SSID = "BroadlinkProv";
static constexpr uint16_t BROADLINK_UDP_PORT = 80;
static constexpr uint8_t BROADLINK_WIFI_SECURITY = 3;  // WPA2
// Mini reboot + DHCP after leaving BroadlinkProv is slow.
static constexpr uint32_t BL_WAIT_STATION_MS = 60000;
static constexpr uint32_t BL_BOOT_WAIT_MS = 12000;       // short attach window at boot
static constexpr uint32_t BL_DHCP_SETTLE_MS = 8000;
static constexpr uint32_t BL_DISCOVER_TRY_MS = 8000;
static constexpr uint32_t BL_BG_DISCOVER_MS = 3000;      // background attach probe
static constexpr int BL_DISCOVER_TRIES = 6;
static constexpr uint32_t BL_ATTACH_RETRY_MS = 8000;     // retry SoftAP attach
static constexpr uint32_t BL_PING_MS = 30000;            // online check while ready
static constexpr int BL_PING_FAILS = 3;                  // mark offline after N fails

