#include "broadlink_rm.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <cerrno>
#include <cstring>
#include <esp_task_wdt.h>
#include <esp_wifi.h>
#include <mbedtls/aes.h>

#include "config.h"

namespace {

constexpr uint8_t kInitKey[16] = {0x09, 0x76, 0x28, 0x34, 0x3f, 0xe9, 0x9e, 0x23,
                                  0x76, 0x5c, 0x15, 0x13, 0xac, 0xcf, 0x8b, 0x02};

void feedWdt() { esp_task_wdt_reset(); }

IPAddress softApBroadcast() {
  // SoftAP is fixed at 192.168.4.0/24 — global 255.255.255.255 fails with errno 118.
  return IPAddress(192, 168, 4, 255);
}

}  // namespace

void BroadlinkRm::progress(const char *msg) {
  if (msg) {
    Serial.printf("[BL] %s\n", msg);
  }
  if (progressFn_) {
    progressFn_(msg);
  }
}

uint16_t BroadlinkRm::checksum(const uint8_t *data, size_t len) {
  uint32_t sum = 0xBEAF;
  for (size_t i = 0; i < len; ++i) {
    sum = (sum + data[i]) & 0xFFFF;
  }
  return static_cast<uint16_t>(sum);
}

void BroadlinkRm::updateAes(const uint8_t *key16) {
  memcpy(aesKey_, key16, 16);
}

void BroadlinkRm::aesEncrypt(const uint8_t *in, size_t len, uint8_t *out) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, aesKey_, 128);
  uint8_t iv[16];
  memcpy(iv, aesIv_, 16);
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, len, iv, in, out);
  mbedtls_aes_free(&ctx);
}

void BroadlinkRm::aesDecrypt(const uint8_t *in, size_t len, uint8_t *out) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_dec(&ctx, aesKey_, 128);
  uint8_t iv[16];
  memcpy(iv, aesIv_, 16);
  mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len, iv, in, out);
  mbedtls_aes_free(&ctx);
}

bool BroadlinkRm::startSoftAp() {
  progress("IR: SoftAP…");
  // BLE is already up — WIFI_PS_NONE aborts coexistence on ESP32-S3.
  // Keep modem sleep so WiFi + NimBLE can share the RF.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(true);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  // Fixed SoftAP IP so discovery local_ip is stable.
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(EMS_WIFI_SSID, EMS_WIFI_PASS)) {
    Serial.println(F("[BL] SoftAP falló"));
    return false;
  }
  Serial.printf("[BL] SoftAP %s  IP=%s  bcast=%s\n", EMS_WIFI_SSID,
                WiFi.softAPIP().toString().c_str(), softApBroadcast().toString().c_str());
  return true;
}

bool BroadlinkRm::connectBroadlinkAp() {
  progress("IR: BroadlinkProv…");
  Serial.printf("[BL] conectando a %s …\n", BROADLINK_AP_SSID);
  WiFi.begin(BROADLINK_AP_SSID);  // open AP
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    feedWdt();
    delay(200);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[BL] no se pudo unir a BroadlinkProv"));
    return false;
  }
  Serial.printf("[BL] STA OK IP=%s\n", WiFi.localIP().toString().c_str());
  return true;
}

bool BroadlinkRm::sendWifiSetup(const char *ssid, const char *pass, uint8_t security) {
  uint8_t payload[0x88];
  memset(payload, 0, sizeof(payload));
  payload[0x26] = 0x14;

  const size_t ssidLen = strnlen(ssid, 32);
  const size_t passLen = strnlen(pass, 32);
  memcpy(&payload[0x44], ssid, ssidLen);
  memcpy(&payload[0x64], pass, passLen);
  payload[0x84] = static_cast<uint8_t>(ssidLen);
  payload[0x85] = static_cast<uint8_t>(passLen);
  payload[0x86] = security;

  const uint16_t cs = checksum(payload, sizeof(payload));
  payload[0x20] = cs & 0xFF;
  payload[0x21] = (cs >> 8) & 0xFF;

  // On BroadlinkProv (STA), use STA subnet broadcast — not 255.255.255.255.
  IPAddress bcast = WiFi.broadcastIP();
  if (bcast == IPAddress(0, 0, 0, 0)) {
    bcast = IPAddress(255, 255, 255, 255);
  }

  WiFiUDP udp;
  udp.begin(0);
  for (int i = 0; i < 5; ++i) {
    feedWdt();
    udp.beginPacket(bcast, BROADLINK_UDP_PORT);
    udp.write(payload, sizeof(payload));
    if (!udp.endPacket()) {
      Serial.printf("[BL] setup UDP fail errno=%d bcast=%s\n", errno, bcast.toString().c_str());
    }
    delay(300);
  }
  udp.stop();
  Serial.printf("[BL] setup WiFi → SSID=%s (WPA2) via %s\n", ssid, bcast.toString().c_str());
  return true;
}

bool BroadlinkRm::discover(uint32_t timeoutMs) {
  WiFiUDP udp;
  const uint16_t localPort = 9998;
  if (!udp.begin(localPort)) {
    Serial.println(F("[BL] UDP bind falló"));
    return false;
  }

  const IPAddress localIp = WiFi.softAPIP();
  const IPAddress bcast = softApBroadcast();
  Serial.printf("[BL] discover bcast=%s stations=%d\n", bcast.toString().c_str(),
                WiFi.softAPgetStationNum());

  uint8_t packet[0x30];
  memset(packet, 0, sizeof(packet));

  const time_t nowSec = time(nullptr);
  struct tm t{};
  gmtime_r(&nowSec, &t);
  const int32_t utcOffHours = 0;
  memcpy(&packet[0x08], &utcOffHours, 4);
  const uint16_t year = t.tm_year > 100 ? static_cast<uint16_t>(1900 + t.tm_year) : 2026;
  packet[0x0C] = year & 0xFF;
  packet[0x0D] = (year >> 8) & 0xFF;
  packet[0x0E] = t.tm_min;
  packet[0x0F] = t.tm_hour;
  packet[0x10] = year % 100;
  packet[0x11] = t.tm_wday == 0 ? 7 : t.tm_wday;
  packet[0x12] = t.tm_mday;
  packet[0x13] = t.tm_mon + 1;

  packet[0x18] = localIp[3];
  packet[0x19] = localIp[2];
  packet[0x1A] = localIp[1];
  packet[0x1B] = localIp[0];
  packet[0x1C] = localPort & 0xFF;
  packet[0x1D] = (localPort >> 8) & 0xFF;
  packet[0x26] = 6;

  const uint16_t cs = checksum(packet, sizeof(packet));
  packet[0x20] = cs & 0xFF;
  packet[0x21] = (cs >> 8) & 0xFF;

  auto tryParse = [&]() -> bool {
    const int n = udp.parsePacket();
    if (n <= 0) {
      return false;
    }
    uint8_t resp[128];
    const int got = udp.read(resp, sizeof(resp));
    if (got < 0x40) {
      return false;
    }
    devType_ = static_cast<uint16_t>(resp[0x34] | (resp[0x35] << 8));
    for (int i = 0; i < 6; ++i) {
      mac_[i] = resp[0x3F - i];
    }
    host_ = udp.remoteIP();
    port_ = BROADLINK_UDP_PORT;
    Serial.printf("[BL] encontrado type=0x%04X IP=%s MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
                  devType_, host_.toString().c_str(), mac_[0], mac_[1], mac_[2], mac_[3],
                  mac_[4], mac_[5]);
    return true;
  };

  const uint32_t start = millis();
  bool found = false;
  uint8_t probeHost = 2;  // DHCP usually starts at .2

  while (millis() - start < timeoutMs) {
    feedWdt();

    // 1) SoftAP subnet broadcast (255.255.255.255 → errno 118 on SoftAP)
    udp.beginPacket(bcast, BROADLINK_UDP_PORT);
    udp.write(packet, sizeof(packet));
    udp.endPacket();

    // 2) Unicast hello sweep — more reliable than broadcast in AP mode
    for (int i = 0; i < 3; ++i) {
      const IPAddress dest(192, 168, 4, probeHost);
      udp.beginPacket(dest, BROADLINK_UDP_PORT);
      udp.write(packet, sizeof(packet));
      udp.endPacket();
      probeHost++;
      if (probeHost > 10) {
        probeHost = 2;
      }
    }

    const uint32_t slice = millis();
    while (millis() - slice < 600) {
      feedWdt();
      if (tryParse()) {
        found = true;
        break;
      }
      delay(20);
    }
    if (found) {
      break;
    }
  }

  udp.stop();
  if (!found) {
    Serial.printf("[BL] discover: sin respuesta (bcast=%s, stations=%d)\n",
                  bcast.toString().c_str(), WiFi.softAPgetStationNum());
  }
  return found;
}

bool BroadlinkRm::sendPacket(uint16_t command, const uint8_t *payload, size_t payloadLen,
                             uint8_t *respOut, size_t respMax, size_t &respLen,
                             uint32_t timeoutMs) {
  count_ = static_cast<uint16_t>(((count_ + 1) | 0x8000) & 0xFFFF);

  size_t pad = (16 - (payloadLen % 16)) % 16;
  const size_t encLen = payloadLen + pad;
  // SmartIR climate blobs are hundreds of bytes + 4B cmd header → need >256.
  constexpr size_t kMaxEnc = 704;
  uint8_t plain[kMaxEnc];
  uint8_t encrypted[kMaxEnc];
  if (encLen > sizeof(plain)) {
    Serial.printf("[BL] payload demasiado grande (%u)\n", static_cast<unsigned>(encLen));
    return false;
  }
  memset(plain, 0, encLen);
  if (payloadLen) {
    memcpy(plain, payload, payloadLen);
  }
  aesEncrypt(plain, encLen, encrypted);

  uint8_t packet[56 + kMaxEnc];
  memset(packet, 0, 0x38);
  packet[0x00] = 0x5a;
  packet[0x01] = 0xa5;
  packet[0x02] = 0xaa;
  packet[0x03] = 0x55;
  packet[0x04] = 0x5a;
  packet[0x05] = 0xa5;
  packet[0x06] = 0xaa;
  packet[0x07] = 0x55;
  packet[0x24] = devType_ & 0xFF;
  packet[0x25] = (devType_ >> 8) & 0xFF;
  packet[0x26] = command & 0xFF;
  packet[0x27] = (command >> 8) & 0xFF;
  packet[0x28] = count_ & 0xFF;
  packet[0x29] = (count_ >> 8) & 0xFF;
  // MAC reversed in header
  for (int i = 0; i < 6; ++i) {
    packet[0x2A + i] = mac_[5 - i];
  }
  packet[0x30] = deviceId_ & 0xFF;
  packet[0x31] = (deviceId_ >> 8) & 0xFF;
  packet[0x32] = (deviceId_ >> 16) & 0xFF;
  packet[0x33] = (deviceId_ >> 24) & 0xFF;

  const uint16_t pCs = checksum(payload, payloadLen);
  packet[0x34] = pCs & 0xFF;
  packet[0x35] = (pCs >> 8) & 0xFF;

  memcpy(packet + 0x38, encrypted, encLen);
  const size_t total = 0x38 + encLen;
  const uint16_t cs = checksum(packet, total);
  packet[0x20] = cs & 0xFF;
  packet[0x21] = (cs >> 8) & 0xFF;

  WiFiUDP udp;
  udp.begin(0);
  const uint32_t start = millis();
  bool ok = false;
  respLen = 0;
  if (timeoutMs < 500) {
    timeoutMs = 500;
  }

  while (millis() - start < timeoutMs) {
    feedWdt();
    udp.beginPacket(host_, port_);
    udp.write(packet, total);
    udp.endPacket();

    const uint32_t slice = millis();
    while (millis() - slice < 400) {
      feedWdt();
      const int n = udp.parsePacket();
      if (n <= 0) {
        delay(10);
        continue;
      }
      respLen = static_cast<size_t>(udp.read(respOut, respMax));
      ok = respLen >= 0x38;
      break;
    }
    if (ok) {
      break;
    }
  }
  udp.stop();
  return ok;
}

bool BroadlinkRm::auth() {
  progress("IR: autenticando…");
  updateAes(kInitKey);
  deviceId_ = 0;
  count_ = static_cast<uint16_t>(random(0x8000, 0xFFFF));

  uint8_t payload[0x50];
  memset(payload, 0, sizeof(payload));
  memset(&payload[0x04], 0x31, 16);
  payload[0x1E] = 0x01;
  payload[0x2D] = 0x01;
  memcpy(&payload[0x30], "Refugio", 7);

  uint8_t resp[256];
  size_t respLen = 0;
  if (!sendPacket(0x65, payload, sizeof(payload), resp, sizeof(resp), respLen)) {
    Serial.println(F("[BL] auth: sin respuesta"));
    return false;
  }

  const uint16_t err = static_cast<uint16_t>(resp[0x22] | (resp[0x23] << 8));
  if (err != 0) {
    Serial.printf("[BL] auth error=%u\n", err);
    return false;
  }

  const size_t encLen = respLen - 0x38;
  if (encLen < 16 || (encLen % 16) != 0) {
    Serial.println(F("[BL] auth: payload inválido"));
    return false;
  }

  uint8_t plain[256];
  aesDecrypt(resp + 0x38, encLen, plain);
  deviceId_ = static_cast<uint32_t>(plain[0] | (plain[1] << 8) | (plain[2] << 16) |
                                   (plain[3] << 24));
  updateAes(&plain[4]);
  Serial.printf("[BL] auth OK id=0x%08lX\n", static_cast<unsigned long>(deviceId_));
  return true;
}

bool BroadlinkRm::waitForSoftApStation(uint32_t timeoutMs) {
  progress("IR: esperando Mini…");
  Serial.printf("[BL] esperando cliente SoftAP (hasta %lus)…\n",
                static_cast<unsigned long>(timeoutMs / 1000));
  const uint32_t start = millis();
  int lastN = -1;
  uint32_t lastUiMs = 0;
  while (millis() - start < timeoutMs) {
    feedWdt();
    const uint32_t elapsed = millis() - start;
    if (elapsed - lastUiMs >= 5000) {
      lastUiMs = elapsed;
      char buf[28];
      snprintf(buf, sizeof(buf), "IR: Mini… %lus",
               static_cast<unsigned long>(elapsed / 1000));
      progress(buf);
    }
    const int n = WiFi.softAPgetStationNum();
    if (n != lastN) {
      lastN = n;
      Serial.printf("[BL] SoftAP clientes=%d\n", n);
    }
    if (n > 0) {
      progress("IR: DHCP…");
      Serial.printf("[BL] cliente visto — esperando DHCP %lus…\n",
                    static_cast<unsigned long>(BL_DHCP_SETTLE_MS / 1000));
      const uint32_t settleStart = millis();
      while (millis() - settleStart < BL_DHCP_SETTLE_MS) {
        feedWdt();
        delay(200);
      }
      return true;
    }
    delay(500);
  }
  progress("IR: Mini timeout");
  Serial.println(F("[BL] timeout: nadie en SoftAP"));
  return false;
}

bool BroadlinkRm::discoverWithRetries() {
  for (int i = 1; i <= BL_DISCOVER_TRIES; ++i) {
    feedWdt();
    char buf[28];
    snprintf(buf, sizeof(buf), "IR: buscando %d/%d", i, BL_DISCOVER_TRIES);
    progress(buf);
    Serial.printf("[BL] discover intento %d/%d…\n", i, BL_DISCOVER_TRIES);
    if (discover(BL_DISCOVER_TRY_MS)) {
      return true;
    }
    // Mini may still be associating — pause before retry.
    const uint32_t pauseStart = millis();
    while (millis() - pauseStart < 3000) {
      feedWdt();
      delay(200);
    }
  }
  return false;
}

bool BroadlinkRm::provisionFromApMode() {
  ready_ = false;
  if (!startSoftAp()) {
    return false;
  }
  softApOk_ = true;
  if (!connectBroadlinkAp()) {
    WiFi.disconnect(true, false);
    return false;
  }

  sendWifiSetup(EMS_WIFI_SSID, EMS_WIFI_PASS, BROADLINK_WIFI_SECURITY);

  Serial.println(F("[BL] setup enviado — Mini reinicia y une a SoftAP…"));
  WiFi.disconnect(true, false);  // leave BroadlinkProv; keep SoftAP
  delay(500);
  // Stay AP+STA (empty STA) — pure WIFI_AP breaks BLE coexistence on S3.
  // Modem sleep must stay on while BLE shares the radio.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(true);
  esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  delay(500);

  if (!waitForSoftApStation(BL_WAIT_STATION_MS)) {
    return false;
  }
  if (!discoverWithRetries()) {
    return false;
  }
  if (!auth()) {
    return false;
  }
  ready_ = true;
  pingFails_ = 0;
  lastPingMs_ = millis();
  progress("IR: listo");
  Serial.println(F("[BL] RM Mini listo (local)"));
  return true;
}

bool BroadlinkRm::tryAttachOnce() {
  if (!softApOk_) {
    return false;
  }
  const int stations = WiFi.softAPgetStationNum();
  if (stations != lastStationCount_) {
    lastStationCount_ = stations;
    Serial.printf("[BL] SoftAP clientes=%d\n", stations);
  }
  if (stations <= 0) {
    return false;
  }

  // Brief settle — Mini may still be getting DHCP.
  const uint32_t settleStart = millis();
  while (millis() - settleStart < 1500) {
    feedWdt();
    delay(100);
  }

  progress("IR: buscando…");
  if (!discover(BL_BG_DISCOVER_MS)) {
    return false;
  }
  if (!auth()) {
    return false;
  }
  ready_ = true;
  pingFails_ = 0;
  lastPingMs_ = millis();
  progress("IR: listo");
  Serial.println(F("[BL] RM Mini unido (fondo)"));
  return true;
}

bool BroadlinkRm::pingDevice() {
  if (!ready_) {
    return false;
  }
  // Same shape as python-broadlink check_sensors — any reply = online.
  uint8_t payload[16];
  memset(payload, 0, sizeof(payload));
  payload[0] = 0x01;
  uint8_t resp[128];
  size_t respLen = 0;
  if (!sendPacket(0x6A, payload, sizeof(payload), resp, sizeof(resp), respLen, 2000)) {
    return false;
  }
  return respLen >= 0x38;
}

void BroadlinkRm::markOffline(const char *why) {
  if (!ready_) {
    return;
  }
  ready_ = false;
  pingFails_ = 0;
  nextAttachMs_ = millis() + 1000;
  Serial.printf("[BL] Mini offline (%s) — esperando rejoin SoftAP\n", why ? why : "?");
}

bool BroadlinkRm::begin() {
  progress("IR: iniciando…");
  Serial.println(F("[BL] iniciando SoftAP (Mini puede unirse luego)"));
  if (!startSoftAp()) {
    progress("IR: SoftAP falló");
    softApOk_ = false;
    return false;
  }
  softApOk_ = true;

  // Short boot window only — do not block minutes if Mini is off.
  if (waitForSoftApStation(BL_BOOT_WAIT_MS)) {
    progress("IR: buscando…");
    if (discover(BL_DISCOVER_TRY_MS) && auth()) {
      ready_ = true;
      pingFails_ = 0;
      lastPingMs_ = millis();
      progress("IR: listo");
      Serial.println(F("[BL] RM Mini listo en arranque"));
      return true;
    }
  }

  progress("IR: SoftAP OK");
  Serial.println(F("[BL] sin Mini aún — SoftAP activo, attach en background"));
  nextAttachMs_ = millis() + BL_ATTACH_RETRY_MS;
  return false;
}

void BroadlinkRm::loop() {
  if (!softApOk_) {
    return;
  }

  const uint32_t now = millis();

  if (ready_) {
    if (now - lastPingMs_ < BL_PING_MS) {
      return;
    }
    lastPingMs_ = now;
    if (pingDevice()) {
      if (pingFails_ != 0) {
        Serial.println(F("[BL] ping OK"));
      }
      pingFails_ = 0;
      return;
    }
    pingFails_++;
    Serial.printf("[BL] ping falló (%d/%d)\n", pingFails_, BL_PING_FAILS);
    if (pingFails_ >= BL_PING_FAILS) {
      markOffline("ping");
    }
    return;
  }

  // Not ready: attach whenever a SoftAP client appears (or retry periodically).
  if (static_cast<int32_t>(now - nextAttachMs_) < 0) {
    return;
  }
  nextAttachMs_ = now + BL_ATTACH_RETRY_MS;

  if (tryAttachOnce()) {
    return;
  }
}

bool BroadlinkRm::sendIr(const uint8_t *data, size_t len, bool fromProgmem) {
  // python-broadlink: packet = [0x02,0x00,0x00,0x00] + ir_blob (starts 0x26…)
  constexpr size_t kMaxIr = 640;
  if (!ready_ || !data || len == 0 || len > kMaxIr) {
    return false;
  }
  uint8_t payload[4 + kMaxIr];
  payload[0] = 0x02;
  payload[1] = 0x00;
  payload[2] = 0x00;
  payload[3] = 0x00;
  if (fromProgmem) {
    memcpy_P(payload + 4, data, len);
  } else {
    memcpy(payload + 4, data, len);
  }

  uint8_t resp[128];
  size_t respLen = 0;
  if (!sendPacket(0x6A, payload, 4 + len, resp, sizeof(resp), respLen)) {
    Serial.println(F("[BL] sendIr: sin respuesta"));
    pingFails_++;
    if (pingFails_ >= BL_PING_FAILS) {
      markOffline("sendIr");
    }
    return false;
  }
  const uint16_t err = static_cast<uint16_t>(resp[0x22] | (resp[0x23] << 8));
  if (err != 0) {
    Serial.printf("[BL] sendIr error=%u\n", err);
    return false;
  }
  pingFails_ = 0;
  lastPingMs_ = millis();
  Serial.printf("[BL] IR enviado (%u bytes)\n", static_cast<unsigned>(len));
  return true;
}

bool BroadlinkRm::enterLearning() {
  if (!ready_) {
    return false;
  }
  // python-broadlink rmmini.enter_learning → _send(0x3)
  const uint8_t payload[4] = {0x03, 0x00, 0x00, 0x00};
  uint8_t resp[128];
  size_t respLen = 0;
  if (!sendPacket(0x6A, payload, sizeof(payload), resp, sizeof(resp), respLen)) {
    Serial.println(F("[BL] enterLearning: sin respuesta"));
    return false;
  }
  const uint16_t err = static_cast<uint16_t>(resp[0x22] | (resp[0x23] << 8));
  if (err != 0) {
    Serial.printf("[BL] enterLearning error=%u\n", err);
    return false;
  }
  pingFails_ = 0;
  lastPingMs_ = millis();
  Serial.println(F("[BL] learn mode ON — apunta el mando al Mini"));
  return true;
}

bool BroadlinkRm::checkLearnedIr(uint8_t *out, size_t outMax, size_t &outLen) {
  outLen = 0;
  if (!ready_ || !out || outMax == 0) {
    return false;
  }
  // python-broadlink rmmini.check_data → _send(0x4); blob = decrypt[4:]
  const uint8_t payload[4] = {0x04, 0x00, 0x00, 0x00};
  // Header 0x38 + encrypted (IR up to ~640 + pad) → need roomy buffer.
  constexpr size_t kRespMax = 896;
  uint8_t resp[kRespMax];
  size_t respLen = 0;
  if (!sendPacket(0x6A, payload, sizeof(payload), resp, sizeof(resp), respLen, 2000)) {
    return false;
  }
  const uint16_t err = static_cast<uint16_t>(resp[0x22] | (resp[0x23] << 8));
  if (err != 0) {
    // No code yet (or empty) — normal while waiting for a button press.
    return false;
  }
  if (respLen < 0x38 + 16) {
    return false;
  }
  const size_t encLen = respLen - 0x38;
  if ((encLen % 16) != 0 || encLen > 704) {
    Serial.println(F("[BL] checkLearnedIr: payload inválido"));
    return false;
  }
  uint8_t plain[704];
  aesDecrypt(resp + 0x38, encLen, plain);
  // Decrypted: [cmd:4][ir_blob…]
  if (encLen < 5) {
    return false;
  }
  const size_t blobLen = encLen - 4;
  // Trim trailing AES padding zeros after the Broadlink length field when present.
  size_t useLen = blobLen;
  if (plain[4] == 0x26 && blobLen >= 4) {
    const size_t declared = 4u + plain[6] + (static_cast<size_t>(plain[7]) << 8);
    if (declared >= 4 && declared <= blobLen) {
      useLen = declared;
    }
  }
  if (useLen == 0 || useLen > outMax) {
    Serial.printf("[BL] checkLearnedIr: blob %u no cabe (max %u)\n",
                  static_cast<unsigned>(useLen), static_cast<unsigned>(outMax));
    return false;
  }
  memcpy(out, plain + 4, useLen);
  outLen = useLen;
  pingFails_ = 0;
  lastPingMs_ = millis();
  return true;
}

bool BroadlinkRm::learnIrBlocking(uint8_t *out, size_t outMax, size_t &outLen,
                                  uint32_t timeoutMs) {
  outLen = 0;
  if (!enterLearning()) {
    return false;
  }
  const uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    feedWdt();
    if (checkLearnedIr(out, outMax, outLen)) {
      Serial.printf("[BL] IR aprendido (%u bytes)\n", static_cast<unsigned>(outLen));
      return true;
    }
    delay(750);
  }
  Serial.println(F("[BL] learn timeout"));
  return false;
}
