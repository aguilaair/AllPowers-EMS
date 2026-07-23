#include "allpowers_ble.h"

#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <esp_coexist.h>
#include <esp_task_wdt.h>

namespace {
static constexpr const char *NOTIFY_UUID = "0000fff1-0000-1000-8000-00805f9b34fb";
static constexpr const char *WRITE_UUID = "0000fff2-0000-1000-8000-00805f9b34fb";
static constexpr const char *SERVICE_UUID = "0000fff0-0000-1000-8000-00805f9b34fb";

AllPowersBle *g_instance = nullptr;
NimBLEClient *g_client = nullptr;
NimBLERemoteCharacteristic *g_writeChar = nullptr;
NimBLERemoteCharacteristic *g_notifyChar = nullptr;
NimBLEAdvertisedDevice *g_adv = nullptr;
NimBLEAddress g_lastAddr;
bool g_haveLastAddr = false;

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient * /*pClient*/) override {
    Serial.println(F("[BLE] connected"));
  }

  void onDisconnect(NimBLEClient * /*pClient*/) override {
    Serial.println(F("[BLE] disconnected"));
    g_writeChar = nullptr;
    g_notifyChar = nullptr;
    if (g_instance) {
      g_instance->scheduleReconnect();
    }
  }
};

class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *advertisedDevice) override {
    const std::string name = advertisedDevice->getName();
    const bool nameMatch =
        !name.empty() &&
        name.rfind(BLE_DEVICE_NAME_PREFIX, 0) == 0;  // starts with
    const bool macMatch = BLE_DEVICE_MAC[0] != '\0' &&
                          advertisedDevice->getAddress().toString() == BLE_DEVICE_MAC;

    if (!nameMatch && !macMatch) {
      return;
    }

    Serial.printf("[BLE] found %s (%s)\n", name.c_str(),
                  advertisedDevice->getAddress().toString().c_str());
    NimBLEDevice::getScan()->stop();
    delete g_adv;
    g_adv = new NimBLEAdvertisedDevice(*advertisedDevice);
    g_lastAddr = advertisedDevice->getAddress();
    g_haveLastAddr = true;
  }
};

ScanCallbacks g_scanCallbacks;
ClientCallbacks g_clientCallbacks;

void notifyCb(NimBLERemoteCharacteristic * /*pChar*/, uint8_t *pData, size_t length,
              bool /*isNotify*/) {
  if (g_instance) {
    g_instance->handleNotify(pData, length);
  }
}

void feedWdt() { esp_task_wdt_reset(); }

void logHex(const char *tag, const uint8_t *data, size_t len) {
  Serial.printf("[BLE] %s len=%u:", tag, static_cast<unsigned>(len));
  const size_t n = len > 24 ? 24 : len;
  for (size_t i = 0; i < n; ++i) {
    Serial.printf(" %02X", data[i]);
  }
  if (len > n) {
    Serial.print(F(" …"));
  }
  Serial.println();
}
}  // namespace

void AllPowersBle::noteStatusRx() {
  lastStatusMs_ = millis();
  staleStrikes_ = 0;
}

void AllPowersBle::handleNotify(const uint8_t *data, size_t len) {
  if (!data || len == 0) {
    return;
  }

  // Reassemble: SoftAP coexistence / small MTU can split frames.
  if (rxLen_ + len > sizeof(rxBuf_)) {
    rxLen_ = 0;
  }
  memcpy(rxBuf_ + rxLen_, data, len);
  rxLen_ += len;

  // Drop leading junk until magic.
  while (rxLen_ >= 2 && !(rxBuf_[0] == 0xA5 && rxBuf_[1] == 0x65)) {
    memmove(rxBuf_, rxBuf_ + 1, --rxLen_);
  }

  while (rxLen_ >= 8) {
    size_t frameLen = AllPowersProto::expectedNotifyLen(rxBuf_, rxLen_);
    if (frameLen == 0) {
      memmove(rxBuf_, rxBuf_ + 1, --rxLen_);
      continue;
    }
    // If length field looks wrong but we already have a classic 16-byte status,
    // decode what we have.
    if (frameLen > sizeof(rxBuf_)) {
      frameLen = 0;
    }
    if (frameLen == 0 || rxLen_ < frameLen) {
      if (rxLen_ >= 16 && rxBuf_[6] == 1) {
        frameLen = 16;
      } else if (rxLen_ >= 14 && rxBuf_[6] == 3) {
        frameLen = 14;
      } else {
        break;  // wait for more bytes
      }
    }

    bool crcOk = true;
    const bool hadConfig = state_.hasConfig;
    const auto result =
        AllPowersProto::decodeNotify(rxBuf_, frameLen, state_, &crcOk);
    if (result == AllPowersProto::DecodeResult::Invalid) {
      logHex("invalid", rxBuf_, frameLen);
      memmove(rxBuf_, rxBuf_ + 1, --rxLen_);
      continue;
    }
    if (!crcOk) {
      logHex("crc?", rxBuf_, frameLen);
    }

    if (result == AllPowersProto::DecodeResult::Status) {
      noteStatusRx();
      Serial.printf("[BLE] status SOC=%u%% AC=%d DC=%d in=%u out=%u\n",
                    state_.powerAmount, state_.acOpen ? 1 : 0, state_.dcOpen ? 1 : 0,
                    state_.inPower, state_.outPower);
      if (statusCb_) {
        statusCb_(state_);
      }
    } else if (result == AllPowersProto::DecodeResult::Config && !hadConfig) {
      Serial.printf("[BLE] config hw=%.1f sw=%.1f\n", state_.hardwareVersion,
                    state_.softwareVersion);
    }

    if (rxLen_ > frameLen) {
      memmove(rxBuf_, rxBuf_ + frameLen, rxLen_ - frameLen);
      rxLen_ -= frameLen;
    } else {
      rxLen_ = 0;
    }
  }
}

void AllPowersBle::scheduleReconnect() {
  connected_ = false;
  connecting_ = false;
  g_writeChar = nullptr;
  g_notifyChar = nullptr;
  rxLen_ = 0;
  state_.hasStatus = false;
  nextReconnectMs_ = millis() + reconnectBackoffMs_;
  reconnectBackoffMs_ = min(reconnectBackoffMs_ * 2, 60000u);
}

bool AllPowersBle::begin() {
  g_instance = this;
  // SoftAP will share RF — prefer BT so battery notifies keep flowing.
  esp_coex_preference_set(ESP_COEX_PREFER_BT);
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(185);
  Serial.println(F("[BLE] scanning..."));
  return scanAndConnect();
}

bool AllPowersBle::ensureClient() {
  if (g_client) {
    if (g_client->isConnected()) {
      g_client->disconnect();
      delay(100);
    }
    return true;
  }
  g_client = NimBLEDevice::createClient();
  if (!g_client) {
    return false;
  }
  g_client->setClientCallbacks(&g_clientCallbacks, false);
  g_client->setConnectTimeout(10);
  return true;
}

bool AllPowersBle::subscribeNotify() {
  if (!g_notifyChar) {
    return false;
  }

  // Force CCCD discovery — NimBLE subscribe() returns true even when CCCD is
  // missing, which leaves us "connected" with zero notifications (SOC=-1).
  g_notifyChar->getDescriptors(true);
  NimBLERemoteDescriptor *cccd =
      g_notifyChar->getDescriptor(NimBLEUUID((uint16_t)0x2902));
  if (!cccd) {
    Serial.println(F("[BLE] CCCD 0x2902 missing — notify imposible"));
    return false;
  }

  // Ignore canNotify() bit — some AllPowers firmwares mis-report properties.
  bool ok = g_notifyChar->subscribe(true, notifyCb, true);
  Serial.printf("[BLE] subscribe notify %s\n", ok ? "OK" : "FAIL");
  if (!ok) {
    ok = g_notifyChar->subscribe(false, notifyCb, true);
    Serial.printf("[BLE] subscribe indicate %s\n", ok ? "OK" : "FAIL");
  }
  if (!ok) {
    const uint16_t enable = 0x0001;
    ok = cccd->writeValue((uint8_t *)&enable, 2, true);
    if (ok) {
      g_notifyChar->subscribe(true, notifyCb, true);
    }
    Serial.printf("[BLE] CCCD write fallback %s\n", ok ? "OK" : "FAIL");
  }
  return ok;
}

bool AllPowersBle::finishConnect() {
  if (!g_client || !g_client->isConnected()) {
    return false;
  }

  // Full attribute discovery so CCCD handles are known before subscribe.
  if (!g_client->discoverAttributes()) {
    Serial.println(F("[BLE] discoverAttributes falló — intentando getService"));
  }

  NimBLERemoteService *svc = g_client->getService(SERVICE_UUID);
  if (!svc) {
    Serial.println(F("[BLE] service missing"));
    g_client->disconnect();
    return false;
  }

  g_notifyChar = svc->getCharacteristic(NOTIFY_UUID);
  g_writeChar = svc->getCharacteristic(WRITE_UUID);
  if (!g_notifyChar || !g_writeChar) {
    Serial.println(F("[BLE] characteristics missing"));
    g_notifyChar = nullptr;
    g_writeChar = nullptr;
    g_client->disconnect();
    return false;
  }

  Serial.printf("[BLE] FFF1 notify=%d indicate=%d read=%d\n",
                g_notifyChar->canNotify() ? 1 : 0, g_notifyChar->canIndicate() ? 1 : 0,
                g_notifyChar->canRead() ? 1 : 0);

  if (!subscribeNotify()) {
    Serial.println(F("[BLE] subscribe falló"));
    g_client->disconnect();
    return false;
  }

  delay(50);  // let CCCD settle before notifies
  connected_ = true;
  connecting_ = false;
  reconnectBackoffMs_ = BLE_RECONNECT_BASE_MS;
  lastStatusMs_ = 0;
  lastStaleActionMs_ = millis();
  staleStrikes_ = 0;
  rxLen_ = 0;
  // Keep last-known AC across reconnect; only clear "fresh" flag.
  state_.hasStatus = false;
  Serial.println(F("[BLE] ready"));

  // Wait for status BEFORE any write. Writing with default acOpen=false would
  // pulse CA off even when the pack already has AC on.
  const uint32_t waitStart = millis();
  while (!state_.hasStatus && millis() - waitStart < 3000) {
    feedWdt();
    delay(50);
  }
  if (state_.hasStatus) {
    Serial.printf("[BLE] primer SOC=%u%% AC=%d DC=%d\n", state_.powerAmount,
                  state_.acOpen ? 1 : 0, state_.dcOpen ? 1 : 0);
    ensureDcOn();  // forces DC only; preserves reported AC flags
  } else {
    Serial.println(F("[BLE] sin status aún — no write (evita apagar CA)"));
  }
  return connected_;
}

bool AllPowersBle::connectKnown() {
  if (!g_haveLastAddr && BLE_DEVICE_MAC[0] == '\0') {
    return false;
  }
  if (!ensureClient()) {
    return false;
  }

  NimBLEAddress addr =
      g_haveLastAddr ? g_lastAddr : NimBLEAddress(std::string(BLE_DEVICE_MAC));
  Serial.printf("[BLE] connect directo %s\n", addr.toString().c_str());
  if (!g_client->connect(addr)) {
    Serial.println(F("[BLE] connect directo falló"));
    return false;
  }
  return finishConnect();
}

bool AllPowersBle::scanAndConnect() {
  connecting_ = true;
  connected_ = false;
  g_writeChar = nullptr;
  g_notifyChar = nullptr;
  feedWdt();

  // Prefer last-known / fixed MAC first — SoftAP coexistence often weakens scans.
  if (connectKnown()) {
    return true;
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&g_scanCallbacks, false);
  scan->setActiveScan(true);
  scan->setInterval(80);
  scan->setWindow(40);
  scan->clearResults();

  delete g_adv;
  g_adv = nullptr;

  // Chunked scan so SoftAP + WDT stay alive.
  const uint32_t scanStart = millis();
  while (millis() - scanStart < 10000 && !g_adv) {
    feedWdt();
    scan->start(2, false);
    delay(50);
  }
  scan->stop();

  if (!g_adv) {
    Serial.println(F("[BLE] device not found"));
    connecting_ = false;
    scheduleReconnect();
    return false;
  }

  if (!ensureClient()) {
    connecting_ = false;
    scheduleReconnect();
    return false;
  }

  if (!g_client->connect(g_adv)) {
    Serial.println(F("[BLE] connect failed"));
    connecting_ = false;
    scheduleReconnect();
    return false;
  }

  if (!finishConnect()) {
    connecting_ = false;
    scheduleReconnect();
    return false;
  }
  return true;
}

void AllPowersBle::loop() {
  if (connecting_) {
    return;
  }

  // Catch zombie links where onDisconnect never fired.
  if (connected_) {
    if (g_client && !g_client->isConnected()) {
      Serial.println(F("[BLE] enlace muerto — reintentando"));
      scheduleReconnect();
      return;
    }

    // Connected but mute: SoftAP often keeps GATT up while notifies die.
    const uint32_t now = millis();
    const bool stale =
        lastStatusMs_ == 0 || (now - lastStatusMs_) > BLE_STATUS_STALE_MS;
    if (stale && (now - lastStaleActionMs_) >= BLE_STATUS_STALE_MS) {
      lastStaleActionMs_ = now;
      staleStrikes_++;
      if (staleStrikes_ == 1) {
        Serial.println(F("[BLE] status stale — resubscribe"));
        if (!subscribeNotify()) {
          Serial.println(F("[BLE] resubscribe falló — reconnect"));
          scheduleReconnect();
          return;
        }
        ensureDcOn();
      } else {
        Serial.println(F("[BLE] status ausente — reconnect"));
        scheduleReconnect();
      }
    }
    return;
  }

  if (millis() < nextReconnectMs_) {
    return;
  }
  Serial.println(F("[BLE] reconnecting..."));
  scanAndConnect();
}

bool AllPowersBle::writeStatus() {
  if (!connected_ || !g_writeChar) {
    return false;
  }
  // Hard rule: EMS is powered from DC — never clear dcOpen
  state_.dcOpen = true;

  uint8_t pkt[9];
  AllPowersProto::encodeStatusWrite(state_, pkt);
  Serial.printf("[BLE] write AC=%d DC=%d crc=%02X\n", state_.acOpen, state_.dcOpen, pkt[8]);
  const bool ok = g_writeChar->writeValue(pkt, 9, false);
  if (!ok) {
    Serial.println(F("[BLE] write falló — programar reconnect"));
    scheduleReconnect();
  }
  return ok;
}

bool AllPowersBle::setAc(bool on) {
  state_.acOpen = on;
  state_.dcOpen = true;
  return writeStatus();
}

bool AllPowersBle::ensureDcOn() {
  // Never write before we know flags — default acOpen=false would clear CA.
  if (!state_.hasStatus) {
    state_.dcOpen = true;
    Serial.println(F("[BLE] ensureDcOn diferido (sin status)"));
    return false;
  }
  if (state_.dcOpen) {
    return true;  // already on; do not rewrite (would be a no-op for DC)
  }
  state_.dcOpen = true;
  return writeStatus();
}
