#include "ems_controller.h"

const char *emsModeName(EmsMode mode) {
  switch (mode) {
    case EmsMode::BootSafe:
      return "BOOT";
    case EmsMode::Monitoring:
      return "MONITOR";
    case EmsMode::Cooling:
      return "COOL";
    case EmsMode::Heating:
      return "HEAT";
    case EmsMode::ReserveLockout:
      return "RESERVE";
    case EmsMode::SensorFault:
      return "SENSOR";
    case EmsMode::BleLost:
      return "BLE_LOST";
    case EmsMode::ManualOn:
      return "MANUAL_ON";
    case EmsMode::ManualOff:
      return "MANUAL_OFF";
    default:
      return "?";
  }
}

const char *emsModeNameEs(EmsMode mode) {
  switch (mode) {
    case EmsMode::BootSafe:
      return "Arranque";
    case EmsMode::Monitoring:
      return "Vigilancia";
    case EmsMode::Cooling:
      return "Refrigeración";
    case EmsMode::Heating:
      return "Calefacción";
    case EmsMode::ReserveLockout:
      return "Reserva";
    case EmsMode::SensorFault:
      return "Sensor";
    case EmsMode::BleLost:
      return "Sin BLE";
    case EmsMode::ManualOn:
      return "Manual ON";
    case EmsMode::ManualOff:
      return "Manual OFF";
    default:
      return "—";
  }
}

const char *climateCmdName(ClimateCmd cmd) {
  switch (cmd) {
    case ClimateCmd::Cool:
      return "COOL";
    case ClimateCmd::Heat:
      return "HEAT";
    case ClimateCmd::Off:
    default:
      return "OFF";
  }
}

void EmsController::begin() {
  snap_.mode = EmsMode::BootSafe;
  snap_.reason = "boot: AC off";
  snap_.reasonEs = "Arranque seguro: clima apagado";
  climateCmd_ = ClimateCmd::Off;
  override_ = AcOverride::Auto;
  allowBelowReserve_ = false;
  climateChangedMs_ = millis() - MIN_AC_OFF_MS;
  reserveLatched_ = false;
  outletCutLatched_ = false;
  outletEmergency_ = false;
  acOutletOn_ = true;
  snap_.acOutletOn = true;
  snap_.outletCut = false;
  snap_.outletEmergency = false;
}

void EmsController::onBattery(const AllPowersProto::DeviceState &st) {
  batt_ = st;
}

void EmsController::onAmbient(const AmbientReading &amb) {
  amb_ = amb;
}

void EmsController::onBleConnected(bool connected) {
  bleOk_ = connected;
}

bool EmsController::wantCooling(float t, int soc) const {
  const bool running =
      climateCmd_ == ClimateCmd::Cool && snap_.mode == EmsMode::Cooling;
  if (running) {
    if (soc < SOC_HOLD_COOL_PCT) {
      return false;
    }
    return t > COOL_OFF_C;
  }
  const int need =
      (t >= COOL_EMERGENCY_C) ? SOC_EMERGENCY_COOL_PCT : SOC_ENABLE_COOL_PCT;
  if (soc < need) {
    return false;
  }
  return t >= COOL_ON_C;
}

bool EmsController::wantHeating(float t, int soc) const {
  const bool running =
      climateCmd_ == ClimateCmd::Heat && snap_.mode == EmsMode::Heating;
  if (running) {
    if (soc < SOC_HOLD_HEAT_PCT) {
      return false;
    }
    return t < HEAT_OFF_C;
  }
  const int need =
      (t <= HEAT_EMERGENCY_C) ? SOC_EMERGENCY_HEAT_PCT : SOC_ENABLE_HEAT_PCT;
  if (soc < need) {
    return false;
  }
  return t <= HEAT_ON_C;
}

bool EmsController::canStartClimate(uint32_t now) const {
  if (climateCmd_ == ClimateCmd::Off && (now - climateChangedMs_) < MIN_AC_OFF_MS) {
    return false;
  }
  return true;
}

bool EmsController::canStopClimate(uint32_t now, bool forceReserve) const {
  if (forceReserve) {
    return true;
  }
  if (climateCmd_ != ClimateCmd::Off && (now - climateChangedMs_) < MIN_AC_ON_MS) {
    return false;
  }
  return true;
}

void EmsController::applyClimate(ClimateCmd cmd, uint32_t now, bool &apply) {
  if (cmd == climateCmd_) {
    return;
  }
  climateCmd_ = cmd;
  climateChangedMs_ = now;
  apply = true;
  Serial.printf("[EMS] clima IR -> %s\n", climateCmdName(cmd));
}

bool EmsController::requestAcOn() {
  if (outletCutLatched_) {
    Serial.println(F("[EMS] clima ON bloqueado (corte CA ≤15%) — usa LUCES"));
    return false;
  }
  const int soc = batt_.hasStatus ? static_cast<int>(batt_.powerAmount) : -1;
  if (soc >= 0 && soc <= SOC_RESERVE_CUT_PCT && !allowBelowReserve_) {
    Serial.println(F("[EMS] clima ON requiere confirmación (SOC<=20%)"));
    return true;  // UI must confirm
  }
  override_ = AcOverride::ForceOn;
  if (soc >= 0 && soc <= SOC_RESERVE_CUT_PCT) {
    allowBelowReserve_ = true;
  }
  forceTick_ = true;
  Serial.println(F("[EMS] anulación: clima ON (frío)"));
  return false;
}

void EmsController::requestAcOff() {
  override_ = AcOverride::ForceOff;
  allowBelowReserve_ = false;
  forceTick_ = true;
  Serial.println(F("[EMS] anulación: clima OFF"));
}

void EmsController::confirmLowSocAcOn() {
  allowBelowReserve_ = true;
  override_ = AcOverride::ForceOn;
  forceTick_ = true;
  Serial.println(F("[EMS] confirmado clima ON bajo reserva"));
}

void EmsController::setAutoMode() {
  override_ = AcOverride::Auto;
  allowBelowReserve_ = false;
  forceTick_ = true;
  Serial.println(F("[EMS] modo automático"));
}

void EmsController::toggleOutletEmergency() {
  if (!outletCutLatched_ && !outletEmergency_) {
    Serial.println(F("[EMS] luces emergencia: sin corte CA activo"));
    return;
  }
  outletEmergency_ = !outletEmergency_;
  forceTick_ = true;
  Serial.printf("[EMS] luces emergencia -> %s\n", outletEmergency_ ? "ON" : "OFF");
}

void EmsController::clearOutletEmergency() {
  if (!outletEmergency_) {
    return;
  }
  outletEmergency_ = false;
  forceTick_ = true;
  Serial.println(F("[EMS] luces emergencia OFF"));
}

bool EmsController::tick(ClimateCmd &cmdOut, bool &outletChanged) {
  outletChanged = false;
  const uint32_t now = millis();
  if (!forceTick_ && lastTickMs_ != 0 && (now - lastTickMs_) < EMS_TICK_MS) {
    cmdOut = climateCmd_;
    return false;
  }
  forceTick_ = false;
  lastTickMs_ = now;

  snap_.bleOk = bleOk_;
  snap_.sensorOk = amb_.valid;
  snap_.ambientC = amb_.temperatureC;
  snap_.humidityPct = amb_.humidityPct;
  snap_.socPct = batt_.hasStatus ? static_cast<int>(batt_.powerAmount) : -1;
  snap_.inWatts = batt_.hasStatus ? static_cast<int>(batt_.inPower) : -1;
  snap_.outWatts = batt_.hasStatus ? static_cast<int>(batt_.outPower) : -1;
  snap_.dcOn = true;
  snap_.acOverride = override_;
  snap_.overrideActive = (override_ != AcOverride::Auto);
  snap_.allowBelowReserve = allowBelowReserve_;

  const bool lowSoc =
      batt_.hasStatus && batt_.powerAmount <= SOC_RESERVE_CUT_PCT;
  snap_.lowSoc = lowSoc;

  // Hard CA cut: stuck-IR failsafe. Overridable only via emergency lights.
  const bool outletCutNow =
      batt_.hasStatus && batt_.powerAmount <= SOC_AC_OUTLET_CUT_PCT;
  if (outletCutNow) {
    outletCutLatched_ = true;
  } else if (outletCutLatched_ && batt_.hasStatus &&
             batt_.powerAmount >= SOC_AC_OUTLET_CLEAR_PCT) {
    outletCutLatched_ = false;
    if (outletEmergency_) {
      outletEmergency_ = false;
      Serial.println(F("[EMS] luces emergencia auto-OFF (SOC recuperado)"));
    }
  }
  const bool wantOutlet =
      bleOk_ && (!outletCutLatched_ || outletEmergency_);
  if (wantOutlet != acOutletOn_) {
    acOutletOn_ = wantOutlet;
    outletChanged = true;
    Serial.printf("[EMS] CA outlet -> %s (SOC=%d emerg=%d)\n",
                  acOutletOn_ ? "ON" : "OFF", snap_.socPct,
                  outletEmergency_ ? 1 : 0);
  }
  snap_.acOutletOn = acOutletOn_;
  snap_.outletCut = outletCutLatched_;
  snap_.outletEmergency = outletEmergency_;

  bool forceClimateOff = false;
  const char *reason = "";
  const char *reasonEs = "";

  if (!bleOk_) {
    snap_.mode = EmsMode::BleLost;
    forceClimateOff = true;
    reason = "BLE lost";
    reasonEs = "Sin conexión BLE";
  } else if (!amb_.valid) {
    snap_.mode = EmsMode::SensorFault;
    forceClimateOff = true;
    reason = "SHT31 fault";
    reasonEs = "Fallo del sensor";
  }

  // Crossing / staying at reserve: auto-cut unless user confirmed below 20%
  if (lowSoc) {
    reserveLatched_ = true;
    if (climateCmd_ != ClimateCmd::Off && !allowBelowReserve_) {
      forceClimateOff = true;
      if (override_ == AcOverride::ForceOn) {
        override_ = AcOverride::ForceOff;
      }
      reason = "SOC reserve cut";
      reasonEs = "Corte clima (≤20%)";
    }
  } else if (reserveLatched_) {
    if (batt_.hasStatus && batt_.powerAmount >= SOC_RESERVE_CLEAR_PCT) {
      reserveLatched_ = false;
      allowBelowReserve_ = false;
    }
  }

  if (!lowSoc) {
    allowBelowReserve_ = false;
  }

  // Below hard outlet cut: kill climate (IR Off). Lights may still be on via emergency.
  if (outletCutLatched_) {
    forceClimateOff = true;
    allowBelowReserve_ = false;
    if (override_ == AcOverride::ForceOn) {
      override_ = AcOverride::ForceOff;
    }
    reason = "SOC AC outlet cut";
    reasonEs = outletEmergency_ ? "Luces emergencia (≤15%)" : "Corte CA (≤15%)";
  }

  bool wantCool = false;
  bool wantHeat = false;
  if (!forceClimateOff && override_ == AcOverride::Auto && amb_.valid &&
      batt_.hasStatus && !lowSoc && !reserveLatched_) {
    wantCool = wantCooling(amb_.temperatureC, batt_.powerAmount);
    wantHeat = wantHeating(amb_.temperatureC, batt_.powerAmount);
    if (wantCool && wantHeat) {
      wantHeat = false;
    }
  }

  ClimateCmd desired = ClimateCmd::Off;
  if (forceClimateOff && !(allowBelowReserve_ && override_ == AcOverride::ForceOn &&
                           !outletCutLatched_)) {
    desired = ClimateCmd::Off;
    if (snap_.mode != EmsMode::BleLost && snap_.mode != EmsMode::SensorFault) {
      snap_.mode = EmsMode::ReserveLockout;
    }
    if (reasonEs[0] == '\0') {
      reasonEs = "Reserva de batería";
      reason = "reserve";
    }
  } else if (override_ == AcOverride::ForceOn) {
    desired = ClimateCmd::Cool;
    snap_.mode = EmsMode::ManualOn;
    reason = "manual cool";
    reasonEs = allowBelowReserve_ ? "Clima manual (reserva)" : "Clima manual (frío)";
  } else if (override_ == AcOverride::ForceOff) {
    desired = ClimateCmd::Off;
    snap_.mode = EmsMode::ManualOff;
    reason = "manual off";
    reasonEs = "Clima apagado manualmente";
  } else if (wantCool) {
    desired = ClimateCmd::Cool;
    snap_.mode = EmsMode::Cooling;
    reason = "cooling";
    reasonEs = "Refrigerando";
  } else if (wantHeat) {
    desired = ClimateCmd::Heat;
    snap_.mode = EmsMode::Heating;
    reason = "heating";
    reasonEs = "Calefactando";
  } else {
    snap_.mode = lowSoc || outletCutLatched_ ? EmsMode::ReserveLockout
                                             : EmsMode::Monitoring;
    reason = "standby";
    reasonEs = (lowSoc || outletCutLatched_) ? "En reserva" : "En espera";
    desired = ClimateCmd::Off;
  }

  // Manual below-reserve ON wins over forceClimateOff (but not over ≤15% outlet cut)
  if (allowBelowReserve_ && override_ == AcOverride::ForceOn && bleOk_ &&
      !outletCutLatched_) {
    desired = ClimateCmd::Cool;
    snap_.mode = EmsMode::ManualOn;
    reason = "manual cool below reserve";
    reasonEs = "Clima manual bajo reserva";
  }

  snap_.climateWanted = wantCool || wantHeat;
  snap_.reason = reason;
  snap_.reasonEs = reasonEs;
  snap_.acOverride = override_;
  snap_.overrideActive = (override_ != AcOverride::Auto);
  snap_.allowBelowReserve = allowBelowReserve_;

  bool apply = false;
  const bool forceStop =
      (forceClimateOff &&
       !(allowBelowReserve_ && override_ == AcOverride::ForceOn && !outletCutLatched_)) ||
      (override_ == AcOverride::ForceOff) || outletCutLatched_;
  const bool forceStart =
      (override_ == AcOverride::ForceOn && !outletCutLatched_);

  if (desired != climateCmd_) {
    const bool starting = (desired != ClimateCmd::Off && climateCmd_ == ClimateCmd::Off);
    const bool stopping = (desired == ClimateCmd::Off && climateCmd_ != ClimateCmd::Off);
    // Cool↔Heat while already on: allow immediately (same compressor session).
    if (starting && !forceStart && !canStartClimate(now)) {
      snap_.reason = "min OFF hold";
      snap_.reasonEs = "Espera mín. apagado";
      desired = climateCmd_;
    } else if (stopping && !forceStop && !canStopClimate(now, forceClimateOff)) {
      snap_.reason = "min ON hold";
      snap_.reasonEs = "Espera mín. encendido";
      desired = climateCmd_;
    } else {
      applyClimate(desired, now, apply);
    }
  }

  snap_.climate = climateCmd_;
  snap_.acOn = (climateCmd_ != ClimateCmd::Off);
  cmdOut = climateCmd_;
  return apply;
}
