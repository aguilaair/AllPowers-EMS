# AllPowers EMS — Cat Shelter Energy Management

Off-grid controller for an **AllPowers R4000** + solar + heat-pump AC, running on a
**Waveshare ESP32-S3-Touch-LCD-2.8C** with an **SHT31-D** ambient sensor and a
**Broadlink RM Mini 3** (local IR).

## What it does

| Rule | Behavior |
|------|----------|
| Cool | Ambient ≥28°C → IR Cool@26; off ≤24°C |
| Heat | Ambient ≤12°C → IR Heat@18; off ≥16°C |
| Normal cool start | SOC ≥**60%** |
| Emergency cool | Ambient ≥**32°C** and SOC ≥**40%** |
| Cool hold | Keep cooling until SOC &lt;**35%** (or temp off) |
| Normal heat start | SOC ≥**50%** |
| Emergency heat | Ambient ≤**10°C** and SOC ≥**35%** |
| Heat hold | Keep heating until SOC &lt;**30%** (or temp off) |
| SOC ≤20% | IR climate **off** (reserve) |
| SOC ≤15% | CA outlet **off** (stuck-IR failsafe); **LUCES** button can override for emergency lights |
| CA restore | Outlet back on at SOC ≥**18%** |
| Compressor | Min **15 min ON** / **10 min OFF** (hard cuts bypass min ON) |
| DC rail | **Always on** — EMS + Broadlink |
| CA rail | Heat pump + emergency lights (normally on) |
| Fail-safe | Boot / BLE loss / SHT31 fault → climate off |

Climate is driven by **IR** (not by cycling the CA breaker), except the ≤15% hard cut.
Auto / On / Off controls are hidden while that cut is active; only **LUCES** remains.

IR codes: learned from Chunghop K-830ES **code 13** (Haier/Yair) into `include/haier_ir.h`.
Stock SmartIR Haier 1320–1322 did not work on site.

## Learn IR codes

With the Mini online (IR LED green):

```bash
pip install -r tools/requirements-learn.txt
python tools/learn_ir.py
# or: python tools/learn_ir.py --port /dev/cu.usbmodemXXXX
```

The script opens serial, sends `learn`, walks you through Cool@26 → Heat@18 → Off,
and writes `firmware/include/haier_ir.h`. Then rebuild and flash.

Manual alternative: serial monitor, type `learn`, paste the dump yourself.

## Hardware

| Item | Connection |
|------|------------|
| Waveshare ESP32-S3-Touch-LCD-2.8C | Near R4000 (BLE range) |
| Power | R4000 **DC** / car port (or USB for bench) |
| SHT31-D VDD | 3V3 on I2C header |
| SHT31-D GND | GND |
| SHT31-D SDA | SDA (**GPIO15**) |
| SHT31-D SCL | SCL (**GPIO7**) |
| SHT31-D ADDR | GND → address `0x44` |
| Heat pump + emergency lights | R4000 **AC** outlets |
| Broadlink RM Mini 3 | Same **DC** rail as EMS (SoftAP `Refugio-EMS`) |

Mount the SHT31 in free shelter air (cats), not under the LCD or on the battery.
Keep Broadlink on DC so IR Off still works before a CA hard cut.

## Build & flash

```bash
cd firmware
pio run -t upload
pio device monitor
```

Board: ESP32-S3, 16MB flash, PSRAM enabled (`platformio.ini`).

Optional: set a fixed BLE MAC in `include/config.h` (`BLE_DEVICE_MAC`).
Tune thresholds in the same file.

## Serial / UI

```text
[STATUS] mode=COOL SOC=92% T=31.2C RH=40% clima=COOL BLE=1 | cooling
```

Boot splash shows BLE / Broadlink attach progress. Main screen has BLE + IR link LEDs.

## Protocol

BLE GATT `FFF0` / notify `FFF1` / write `FFF2`, protocol **v2**, XOR CRC.
See `docs/protocol-notes.md` and the official debug HTML under `docs/`.

## First bench checklist

1. I2C: SHT31 found; temp/humidity look sane.
2. BLE: finds `AP R4000 V3.0` (or V2.0); SOC updates; DC + CA stay on when commanded.
3. Broadlink: joins SoftAP (boot or later); IR LED green; ping OK in logs.
4. Manual **On** sends Cool@26; **Off** sends IR Off (point Mini at the AC).
5. Confirm ≤15% shows **LUCES** and hides Auto/On/Off.
6. Confirm min ON/OFF holds (or lower timers for bench).
