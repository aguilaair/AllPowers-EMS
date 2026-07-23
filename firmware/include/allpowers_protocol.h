#pragma once

#include <Arduino.h>
#include <cstring>

// Official AllPowers BLE protocol v2 (from AllPowers Web Bluetooth debug tool).
// Do NOT use community "113 - byte7" checksum — use XOR CRC.

namespace AllPowersProto {

struct DeviceState {
  bool dcOpen = false;
  bool acOpen = false;
  bool is60Hz = false;
  bool beepOpen = false;
  bool ledOpen = false;
  bool screenOpen = false;
  bool voiceOpen = false;
  bool closeBle = false;

  uint8_t powerAmount = 0;
  uint16_t inPower = 0;
  uint16_t outPower = 0;
  uint16_t minutesRemain = 0;

  bool ecoMode = false;
  uint8_t chargingMode = 0;
  bool acMode = false;
  bool carPortEn = false;
  uint8_t ecoTime = 0;
  uint16_t chargeTime = 0;
  float hardwareVersion = 0;
  float softwareVersion = 0;

  bool hasStatus = false;
  bool hasConfig = false;
};

inline uint8_t crcXor(const uint8_t *buf, size_t len) {
  // XOR of bytes [0 .. len-2]; last byte is CRC itself
  if (len < 2) {
    return 0;
  }
  uint8_t c = buf[0];
  for (size_t i = 1; i < len - 1; ++i) {
    c ^= buf[i];
  }
  return c;
}

inline bool valueAtBit(uint8_t e, int t) {
  // 1-indexed bit (matches official JS valueAtBit)
  return ((e >> (t - 1)) & 1) == 1;
}

inline uint8_t valueAtIndex2Bit(uint8_t e, int t) {
  return (e >> (t - 1)) & 3;
}

enum class DecodeResult { Invalid, Status, Config, Other };

// Expected total length from header byte t[5] (payload len). 0 if header too short.
inline size_t expectedNotifyLen(const uint8_t *t, size_t length) {
  if (length < 6 || t[0] != 0xA5 || t[1] != 0x65) {
    return 0;
  }
  return static_cast<size_t>(t[5]) + 8;
}

inline DecodeResult decodeNotify(const uint8_t *t, size_t length, DeviceState &out,
                                 bool *crcOk = nullptr) {
  if (crcOk) {
    *crcOk = true;
  }
  if (length < 8 || t[0] != 0xA5 || t[1] != 0x65) {
    return DecodeResult::Invalid;
  }

  const size_t expect = expectedNotifyLen(t, length);
  // Accept exact length, or longer frames that still contain a full payload
  // (some stacks pad; community parsers ignore length/CRC entirely).
  if (expect == 0 || length < expect) {
    // Still try status/config if the fixed layout fits (R4000 dumps sometimes
    // disagree with t[5] while SOC/flags are at the usual offsets).
    if (!(length >= 16 && t[6] == 1) && !(length >= 14 && t[6] == 3)) {
      return DecodeResult::Invalid;
    }
  } else if (length != expect && length > expect) {
    length = expect;
  }

  const bool goodCrc = t[length - 1] == crcXor(t, length);
  if (crcOk) {
    *crcOk = goodCrc;
  }
  // Do not hard-reject on CRC: real packs (and ESPHome sample dumps) sometimes
  // fail XOR while the official layout is still valid. Writes still use XOR.

  switch (t[6]) {
    case 1: {
      if (length < 16) {
        return DecodeResult::Invalid;
      }
      out.dcOpen = valueAtBit(t[7], 1);
      out.acOpen = valueAtBit(t[7], 2);
      out.is60Hz = valueAtBit(t[7], 3);
      out.beepOpen = valueAtBit(t[7], 4);
      out.ledOpen = valueAtBit(t[7], 5);
      out.screenOpen = valueAtBit(t[7], 6);
      out.voiceOpen = valueAtBit(t[7], 7);
      out.powerAmount = t[8];
      out.inPower = static_cast<uint16_t>((t[9] << 8) | t[10]);
      out.outPower = static_cast<uint16_t>((t[11] << 8) | t[12]);
      out.minutesRemain = static_cast<uint16_t>((t[13] << 8) | t[14]);
      out.hasStatus = true;
      return DecodeResult::Status;
    }
    case 3: {
      if (length < 14) {
        return DecodeResult::Invalid;
      }
      out.ecoMode = valueAtBit(t[7], 1);
      out.chargingMode = valueAtIndex2Bit(t[7], 2);
      out.acMode = valueAtBit(t[7], 4);
      out.carPortEn = valueAtBit(t[7], 5);
      out.ecoTime = t[8];
      out.chargeTime = static_cast<uint16_t>((t[9] << 8) | t[10]);
      // Official tool uses toString(16)/10 — keep as raw/10 for display
      out.hardwareVersion = static_cast<float>(t[11]) / 10.0f;
      out.softwareVersion = static_cast<float>(t[12]) / 10.0f;
      out.hasConfig = true;
      return DecodeResult::Config;
    }
    default:
      return DecodeResult::Other;
  }
}

// encodeNewBle — 9-byte write packet (preserves full status flags)
inline size_t encodeStatusWrite(const DeviceState &s, uint8_t out[9]) {
  uint8_t status = 0;
  if (s.dcOpen) {
    status |= (1 << 0);
  }
  if (s.acOpen) {
    status |= (1 << 1);
  }
  if (s.closeBle) {
    status |= (1 << 2);
  }
  if (s.is60Hz) {
    status |= (1 << 3);
  }
  if (s.beepOpen) {
    status |= (1 << 4);
  }
  if (s.ledOpen) {
    status |= (1 << 5);
  }
  if (s.screenOpen) {
    status |= (1 << 6);
  }
  if (s.voiceOpen) {
    status |= (1 << 7);
  }

  out[0] = 0xA5;
  out[1] = 0x65;
  out[2] = 0x00;
  out[3] = 0xB1;
  out[4] = 0x01;
  out[5] = 0x01;
  out[6] = 0x00;
  out[7] = status;
  out[8] = 0;
  out[8] = crcXor(out, 9);
  return 9;
}

}  // namespace AllPowersProto
