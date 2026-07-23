# AllPowers EMS

Off-grid energy management for a cat shelter: **AllPowers R4000** + solar + heat-pump AC,
controlled by a **Waveshare ESP32-S3-Touch-LCD-2.8C**, **SHT31-D**, and **Broadlink RM Mini 3**.

Firmware lives in [`firmware/`](firmware/) — see [`firmware/README.md`](firmware/README.md).

## Quick start

```bash
cd firmware
pio run -t upload
pio device monitor
```

## Design (short)

- Climate via **IR** (learned Chunghop code 13 / Haier-Yair); CA outlet stays on for AC + emergency lights
- Start cool/heat only with a healthy pack; emergency starts for extreme ambient; hold lower while running
- **≤20%** stops IR climate; **≤15%** hard-cuts CA (stuck-IR failsafe) with on-screen **LUCES** override
- **DC always on** (EMS + Broadlink); compressor **min 15 / 10 min** on/off
- Official AllPowers BLE **v2** protocol (XOR CRC) — see [`docs/protocol-notes.md`](docs/protocol-notes.md)
