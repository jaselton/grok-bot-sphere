#include "ota.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>

static volatile bool offered = false;
static String offVersion, offMd5, failedVersion;
static uint32_t offSize = 0;

// Keep the bootloader's rollback armed after an OTA: the image is marked good by ota_mark_good_if_due().
extern "C" bool verifyRollbackLater() { return true; }

void ota_offer(const char* version, uint32_t size, const char* md5) {
  if (!version || !md5 || strlen(md5) != 32 || !size || size > 0x300000) return;
  if (failedVersion == version) return;               // one try per boot for a version that failed
  offVersion = version; offMd5 = md5; offSize = size; offered = true;
  Serial.printf("[ota] offered %s (%lu bytes), running %s\n", version, (unsigned long)size, FW_VERSION);
}
bool ota_pending() { return offered; }

void ota_mark_good_if_due(bool relayConnected) {
  static bool done = false;
  if (done || !(relayConnected || millis() > 120000)) return;
  done = true;
  const esp_partition_t* p = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(p, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("[ota] %s on %s marked good\n", FW_VERSION, p->label);
  }
}

void ota_run(const String& relayWsUrl, const String& token) {
  offered = false;
  String base = relayWsUrl; base.replace("wss://", "https://"); base.replace("/ws", "");
  String url = base + "/fw/bin?token=" + token;
  Serial.printf("[ota] downloading %s (internal free %u)\n", offVersion.c_str(), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  WiFiClientSecure tls; tls.setInsecure();            // same trust as the relay WebSocket; integrity comes from the MD5
  HTTPClient http; http.setTimeout(20000);
  bool ok = false;
  if (http.begin(tls, url)) {
    int code = http.GET();
    int len = http.getSize();
    if (code == 200 && (len == (int)offSize || len < 0) && Update.begin(offSize) && Update.setMD5(offMd5.c_str())) {
      WiFiClient* s = http.getStreamPtr();
      static uint8_t buf[4096]; size_t got = 0; uint32_t lastData = millis(), lastPct = 0;
      while (got < offSize && millis() - lastData < 20000) {
        size_t n = s->available();
        if (!n) { delay(2); continue; }
        n = s->readBytes(buf, min(n, sizeof buf));
        if (Update.write(buf, n) != n) break;
        got += n; lastData = millis();
        uint32_t pct = got * 100 / offSize; if (pct >= lastPct + 10) { lastPct = pct; Serial.printf("[ota] %lu%%\n", (unsigned long)pct); }
      }
      ok = got == offSize && Update.end();
      if (!ok) Serial.printf("[ota] failed after %u bytes: %s\n", (unsigned)got, Update.errorString());
      if (!ok) Update.abort();
    } else Serial.printf("[ota] http %d size %d (want %lu) %s\n", code, len, (unsigned long)offSize, Update.errorString());
    http.end();
  }
  if (ok) { Serial.printf("[ota] %s written and verified, restarting\n", offVersion.c_str()); delay(300); ESP.restart(); }
  failedVersion = offVersion;
}
