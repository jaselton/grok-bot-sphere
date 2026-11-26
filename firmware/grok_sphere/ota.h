#pragma once
#include <Arduino.h>
// Over-the-air updates through the relay. The relay offers {"ota":{"version","size","md5"}} when it holds firmware
// newer than FW_VERSION; the sphere downloads https://<relay>/fw/bin?token=.. while idle, checks the MD5, and restarts.
// The new image must prove itself (relay connected, or 2 minutes up without a crash), or the bootloader rolls back.
void ota_offer(const char* version, uint32_t size, const char* md5);   // from relay_client (WebSocket task context)
bool ota_pending();
void ota_run(const String& relayWsUrl, const String& token);         // blocking; restarts on success
void ota_mark_good_if_due(bool relayConnected);                      // call from loop()
