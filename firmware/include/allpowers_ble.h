#pragma once

#include "allpowers_protocol.h"
#include "config.h"

#include <functional>

class AllPowersBle {
 public:
  using StatusCallback = std::function<void(const AllPowersProto::DeviceState &)>;

  bool begin();
  void loop();

  bool isConnected() const { return connected_; }
  const AllPowersProto::DeviceState &state() const { return state_; }

  // Merges into last known state. Always forces dcOpen=true (EMS on DC rail).
  // ensureDcOn() never invents AC=off — it waits for a status notify first.
  bool setAc(bool on);
  bool ensureDcOn();

  void onStatus(StatusCallback cb) { statusCb_ = std::move(cb); }

  // Called from NimBLE callbacks
  void handleNotify(const uint8_t *data, size_t len);
  void scheduleReconnect();

 private:
  bool scanAndConnect();
  bool connectKnown();
  bool ensureClient();
  bool finishConnect();
  bool subscribeNotify();
  bool writeStatus();
  void noteStatusRx();

  AllPowersProto::DeviceState state_{};
  StatusCallback statusCb_;
  bool connected_ = false;
  bool connecting_ = false;
  uint32_t nextReconnectMs_ = 0;
  uint32_t reconnectBackoffMs_ = BLE_RECONNECT_BASE_MS;
  uint32_t lastStatusMs_ = 0;
  uint32_t lastStaleActionMs_ = 0;
  uint8_t staleStrikes_ = 0;
  uint8_t rxBuf_[64]{};
  size_t rxLen_ = 0;
};
