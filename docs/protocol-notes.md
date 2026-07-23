# AllPowers BLE protocol notes (R4000)

Authoritative source: official Web Bluetooth debug tool (`allpowers-ble-debug.html`).

## Device

| Field | Value |
|-------|-------|
| Advertised name | `AP R4000 V3.0` (list also has `AP R4000 V2.0`) |
| Protocol | `bleProtocolVersion: 2` |
| Service | `0000fff0-0000-1000-8000-00805f9b34fb` |
| Notify | `0000fff1-...` |
| Write | `0000fff2-...` (write without response) |

## CRC

XOR of all bytes except the last; last byte is CRC.

## Command 1 status (16 bytes)

| Index | Meaning |
|-------|---------|
| 7 | flags: bit1 DC, bit2 AC, bit3 60Hz, bit4 beep, bit5 LED, bit6 screen, bit7 voice (1-indexed) |
| 8 | SOC % |
| 9–10 | input watts |
| 11–12 | output watts |
| 13–14 | minutes remaining |

## Write encodeNewBle (9 bytes)

`A5 65 00 B1 01 01 00 <status> <crc>`

Always merge with last known flags. **This firmware forces DC on** on every write.

## Bench log

Record real V3.0 notification hex dumps below after first connect:

```
(paste hex here)
```
