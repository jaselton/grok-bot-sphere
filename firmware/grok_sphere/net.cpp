#include "net.h"
#include "config.h"
#include "secrets.h"   // RELAY_URL, DEVICE_TOKEN (generated, git-ignored)
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_timer.h>
#include <ArduinoJson.h>

static Preferences prefs;
static WebServer web(80);
static DNSServer dns;
static bool portalOn = false;

bool net_boot_counter_check() {
  prefs.begin("sphere", false);
  // Only real unplug/replug power-ups count toward the 3-cycle reset. Crashes, watchdog and software restarts
  // (flashing, saving Wi-Fi) must never wipe credentials.
  if (esp_reset_reason() != ESP_RST_POWERON) { prefs.putUChar("boots", 0); return false; }
  uint8_t n = prefs.getUChar("boots", 0) + 1;       // each power-up while the counter is still armed counts
  prefs.putUChar("boots", n);
  return n >= BOOT_COUNT_RESET;
}
static void clear_boots(void*) { Preferences p; p.begin("sphere", false); p.putUChar("boots", 0); p.end(); }
void net_boot_counter_clear_later() {
  static esp_timer_handle_t t; esp_timer_create_args_t a = {}; a.callback = clear_boots; a.name = "bootclr";
  esp_timer_create(&a, &t); esp_timer_start_once(t, (uint64_t)BOOT_COUNT_WINDOW_MS * 1000);
}
void net_wipe_wifi() {
  prefs.putString("ssid", ""); prefs.putString("pass", ""); prefs.putUChar("boots", 0);
}
bool net_has_credentials() { return prefs.getString("ssid", "").length() > 0; }
String net_relay_url()    { return prefs.getString("relay", RELAY_URL); }
String net_device_token() { return prefs.getString("token", DEVICE_TOKEN); }
String net_tz()           { return prefs.getString("tz", "CST6CDT,M3.2.0,M11.1.0"); }

static const char PAGE[] PROGMEM = R"(<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<body style="background:#000;color:#fff;font-family:system-ui;max-width:420px;margin:2em auto;padding:0 1em">
<h1 style="color:#39ff14">Set up Grok Sphere</h1>
<form method=POST action=/save>
Wi-Fi name<br><input name=ssid style="width:100%;font-size:1.2em"><br><br>
Wi-Fi password<br><input name=pass type=password style="width:100%;font-size:1.2em"><br><br>
<input type=hidden name=token value="%TOKEN%">
<button style="font-size:1.3em;padding:.6em 1.2em;background:#39ff14;border:0;border-radius:2em">Connect</button></form>
<p style="color:#888">Tip: unplug and plug the sphere 3 times to come back here.</p>)";

void net_start_portal() {
  String id = String((uint32_t)(ESP.getEfuseMac() >> 24), HEX); id.toUpperCase();
  String ssid = String(PORTAL_SSID_PREFIX) + id.substring(0, 4);
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(ssid.c_str());
  Serial.printf("[portal] softAP %s ok=%d ip=%s ch=%d\n", ssid.c_str(), ok, WiFi.softAPIP().toString().c_str(), WiFi.channel());
  dns.start(53, "*", WiFi.softAPIP());                       // captive portal: every name -> us
  web.on("/save", HTTP_POST, [] {
    prefs.putString("ssid", web.arg("ssid")); prefs.putString("pass", web.arg("pass"));
    if (web.arg("token").length()) prefs.putString("token", web.arg("token"));
    web.send(200, "text/html", "<body style='background:#000;color:#fff'>Saved. Restarting...");
    delay(800); ESP.restart();
  });
  web.onNotFound([] {
    Serial.printf("[portal] http %s %s\n", web.hostHeader().c_str(), web.uri().c_str()); String p = FPSTR(PAGE); p.replace("%TOKEN%", prefs.getString("token", "")); web.send(200, "text/html", p); });
  web.begin(); portalOn = true;
}
void net_connect_blocking(uint32_t ms) {
  WiFi.mode(WIFI_STA); WiFi.begin(prefs.getString("ssid").c_str(), prefs.getString("pass").c_str());
  uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) delay(200);
  Serial.printf("[wifi] %s \"%s\" status=%d ip=%s rssi=%d\n", WiFi.status() == WL_CONNECTED ? "connected to" : "FAILED to join",
                prefs.getString("ssid").c_str(), (int)WiFi.status(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
}
void net_loop() {
  if (portalOn) { dns.processNextRequest(); web.handleClient(); return; }
  static uint32_t last; if (WiFi.status() != WL_CONNECTED && millis() - last > 10000) { last = millis(); WiFi.reconnect(); }
}

// ---- USB setup (tools/sphere-wifi.sh). Credentials arrive over the cable, are stored in NVS, never echoed. ----
void net_serial_command(const String& line) {
  if (line == "WIFI?") {
    Serial.printf("[wifi] ssid=\"%s\" status=%d ip=%s rssi=%d\n", prefs.getString("ssid", "").c_str(),
                  (int)WiFi.status(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else if (line == "WIFI-FORGET") {
    net_wipe_wifi(); Serial.println("[wifi] OK forgotten, restarting into setup mode"); delay(300); ESP.restart();
  } else if (line.startsWith("WIFI ")) {
    JsonDocument d;
    if (deserializeJson(d, line.substring(5)) || !d["ssid"].is<const char*>()) { Serial.println("[wifi] ERR bad command"); return; }
    String ssid = d["ssid"].as<String>(), pass = d["pass"] | "";
    if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63) { Serial.println("[wifi] ERR ssid/password length"); return; }
    prefs.putString("ssid", ssid); prefs.putString("pass", pass);
    Serial.printf("[wifi] OK saved \"%s\", restarting to connect\n", ssid.c_str());
    delay(300); ESP.restart();
  }
}
