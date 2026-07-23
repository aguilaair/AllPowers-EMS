#pragma once

#include "allpowers_protocol.h"
#include "config.h"
#include "sht31_sensor.h"

enum class EmsMode : uint8_t {
  BootSafe,
  Monitoring,
  Cooling,
  Heating,
  ReserveLockout,
  SensorFault,
  BleLost,
  ManualOn,
  ManualOff,
};

enum class AcOverride : int8_t {
  Auto = 0,
  ForceOff = -1,
  ForceOn = 1,
};

// IR climate command (CA outlet normally powered; Mini drives the AC).
enum class ClimateCmd : uint8_t {
  Off = 0,
  Cool = 1,  // SmartIR cool / auto / IR_COOL_SETPOINT_C
  Heat = 2,  // SmartIR heat / auto / IR_HEAT_SETPOINT_C
};

const char *emsModeName(EmsMode mode);    // short EN (serial)
const char *emsModeNameEs(EmsMode mode);  // Spain Spanish (UI)
const char *climateCmdName(ClimateCmd cmd);

struct EmsSnapshot {
  EmsMode mode = EmsMode::BootSafe;
  float ambientC = NAN;
  float humidityPct = NAN;
  int socPct = -1;
  int inWatts = -1;
  int outWatts = -1;
  bool acOn = false;  // climate active (Cool/Heat), not outlet power
  bool acOutletOn = true;  // R4000 CA rail desired
  bool outletCut = false;  // ≤15% hard-cut latched
  bool outletEmergency = false;  // user override for emergency lights
  ClimateCmd climate = ClimateCmd::Off;
  bool dcOn = false;
  bool bleOk = false;
  bool sensorOk = false;
  bool climateWanted = false;
  bool lowSoc = false;
  bool overrideActive = false;
  bool allowBelowReserve = false;
  AcOverride acOverride = AcOverride::Auto;
  const char *reason = "";
  const char *reasonEs = "";
};

class EmsController {
 public:
  void begin();
  void onBattery(const AllPowersProto::DeviceState &st);
  void onAmbient(const AmbientReading &amb);
  void onBleConnected(bool connected);

  // Manual climate control from UI (ForceOn = Cool).
  // Returns true if a low-SOC confirmation is required (no change yet).
  bool requestAcOn();
  void requestAcOff();
  void confirmLowSocAcOn();
  void setAutoMode();

  // Force CA outlet ON for emergency lights despite ≤15% hard cut.
  // No-op unless outlet cut is active (or emergency already on — then clears).
  void toggleOutletEmergency();
  void clearOutletEmergency();

  // Returns true if the IR climate command changed and should be sent.
  // outletChanged is set when CA rail desired state flips.
  bool tick(ClimateCmd &cmdOut, bool &outletChanged);

  ClimateCmd climateCommanded() const { return climateCmd_; }
  bool acOutletDesired() const { return acOutletOn_; }
  EmsSnapshot snapshot() const { return snap_; }

 private:
  bool wantCooling(float t, int soc) const;
  bool wantHeating(float t, int soc) const;
  bool canStartClimate(uint32_t now) const;
  bool canStopClimate(uint32_t now, bool forceReserve) const;
  void applyClimate(ClimateCmd cmd, uint32_t now, bool &apply);

  EmsSnapshot snap_{};
  AllPowersProto::DeviceState batt_{};
  AmbientReading amb_{};
  bool bleOk_ = false;
  bool reserveLatched_ = false;
  bool outletCutLatched_ = false;
  bool outletEmergency_ = false;
  bool acOutletOn_ = true;
  ClimateCmd climateCmd_ = ClimateCmd::Off;
  bool allowBelowReserve_ = false;
  AcOverride override_ = AcOverride::Auto;
  bool forceTick_ = false;
  uint32_t climateChangedMs_ = 0;
  uint32_t lastTickMs_ = 0;
};
