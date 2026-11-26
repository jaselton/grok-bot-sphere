#pragma once
#include <Arduino.h>
bool   net_boot_counter_check();        // increments NVS counter; true if >= BOOT_COUNT_RESET quick boots
void   net_boot_counter_clear_later();  // arm a one-shot timer that zeroes the counter after BOOT_COUNT_WINDOW_MS
void   net_wipe_wifi();
bool   net_has_credentials();
void   net_start_portal();              // SoftAP "Charlie-XXXX" + captive DNS + form (network picker); keeps retrying saved Wi-Fi
bool   net_connected();
void   net_connect_blocking(uint32_t ms);
void   net_loop();
String net_relay_url();                 // e.g. wss://grok-sphere-relay.<acct>.workers.dev/ws
String net_device_token();
String net_tz();                        // POSIX TZ string, default CST6CDT,M3.2.0,M11.1.0
void   net_serial_command(const String& line);   // USB setup: WIFI {"ssid":"..","pass":".."} | WIFI? | WIFI-FORGET
