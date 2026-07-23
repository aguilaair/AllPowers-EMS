#pragma once

#include <Arduino.h>

// Local Broadlink RM Mini control (UDP/AES, no cloud).
// SoftAP stays up: Mini can join any time after boot. loop() attaches + pings.
class BroadlinkRm {
 public:
  using ProgressFn = void (*)(const char *msg);

  void onProgress(ProgressFn fn) { progressFn_ = fn; }

  // Starts SoftAP and tries a short attach. Always leaves SoftAP running.
  // Returns true if Mini was found during the boot window.
  bool begin();
  void loop();

  bool isReady() const { return ready_; }

  // Send a raw Broadlink IR packet (typically starts with 0x26… from SmartIR).
  // `data` may live in PROGMEM — copied before encrypt/send.
  bool sendIr(const uint8_t *data, size_t len, bool fromProgmem = true);

  // IR learn (python-broadlink enter_learning / check_data).
  bool enterLearning();
  // Returns true when a blob is ready; false while waiting or on error.
  bool checkLearnedIr(uint8_t *out, size_t outMax, size_t &outLen);
  // enterLearning + poll until blob or timeout. Feeds WDT while waiting.
  bool learnIrBlocking(uint8_t *out, size_t outMax, size_t &outLen,
                       uint32_t timeoutMs = 30000);

  // Force re-provision (Mini must be in BroadlinkProv AP mode).
  bool provisionFromApMode();

 private:
  void progress(const char *msg);
  bool startSoftAp();
  bool connectBroadlinkAp();
  bool sendWifiSetup(const char *ssid, const char *pass, uint8_t security);
  bool waitForSoftApStation(uint32_t timeoutMs);
  bool discover(uint32_t timeoutMs);
  bool discoverWithRetries();
  bool auth();
  bool tryAttachOnce();
  bool pingDevice();
  void markOffline(const char *why);
  bool sendPacket(uint16_t command, const uint8_t *payload, size_t payloadLen,
                  uint8_t *respOut, size_t respMax, size_t &respLen,
                  uint32_t timeoutMs = 5000);
  void updateAes(const uint8_t *key16);
  void aesEncrypt(const uint8_t *in, size_t len, uint8_t *out);
  void aesDecrypt(const uint8_t *in, size_t len, uint8_t *out);
  static uint16_t checksum(const uint8_t *data, size_t len);

  ProgressFn progressFn_ = nullptr;
  bool ready_ = false;
  bool softApOk_ = false;
  uint16_t devType_ = 0;
  uint8_t mac_[6] = {};
  IPAddress host_;
  uint16_t port_ = 80;
  uint32_t deviceId_ = 0;
  uint16_t count_ = 0;
  uint8_t aesKey_[16] = {};
  uint8_t aesIv_[16] = {0x56, 0x2e, 0x17, 0x99, 0x6d, 0x09, 0x3d, 0x28,
                        0xdd, 0xb3, 0xba, 0x69, 0x5a, 0x2e, 0x6f, 0x58};
  uint32_t lastPingMs_ = 0;
  uint32_t nextAttachMs_ = 0;
  int pingFails_ = 0;
  int lastStationCount_ = -1;
};
