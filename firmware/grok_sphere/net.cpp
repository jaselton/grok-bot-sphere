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
<h1 style="color:#39ff14">Set up Charlie</h1>%NOTE%
<form method=POST action=/save>
Wi-Fi name (pick or type)<br><input name=ssid list=nets autocomplete=off style="width:100%;font-size:1.2em"><datalist id=nets>%NETS%</datalist><br><br>
Wi-Fi password<br><input name=pass id=pw type=password style="width:100%;font-size:1.2em"><br>
<label><input type=checkbox onclick="pw.type=this.checked?'text':'password'"> show password</label><br><br>
<input type=hidden name=token value="%TOKEN%">
<button style="font-size:1.3em;padding:.6em 1.2em;background:#39ff14;border:0;border-radius:2em">Connect</button></form>
<p style="color:#888">Tip: unplug and plug Charlie in 3 times to come back here.</p>)";
static String netOptions, savedSsid;
static uint32_t lastRetry = 0;
static void scan_networks() {                       // the picker list: strongest first, no duplicates/hidden
  int n = WiFi.scanNetworks(); netOptions = ""; String seen = "\n";
  for (int i = 0; i < n && i < 40; i++) {
    int best = -1;
    for (int j = 0; j < n; j++) if (WiFi.SSID(j).length() && seen.indexOf("\n" + WiFi.SSID(j) + "\n") < 0 && (best < 0 || WiFi.RSSI(j) > WiFi.RSSI(best))) best = j;
    if (best < 0) break;
    String s = WiFi.SSID(best); seen += s + "\n";
    s.replace("&", "&amp;"); s.replace("\"", "&quot;"); s.replace("<", "&lt;");
    netOptions += "<option value=\"" + s + "\">";
  }
  WiFi.scanDelete();
  Serial.printf("[portal] scan: %d networks\n", n);
}

void net_start_portal() {
  String id = String((uint32_t)(ESP.getEfuseMac() >> 24), HEX); id.toUpperCase();
  String ssid = String(PORTAL_SSID_PREFIX) + id.substring(0, 4);
  savedSsid = prefs.getString("ssid", "");
  WiFi.mode(WIFI_AP_STA);                                    // STA side: scan, and keep trying the saved network
  WiFi.disconnect(); scan_networks();
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
    Serial.printf("[portal] http %s %s\n", web.hostHeader().c_str(), web.uri().c_str()); String p = FPSTR(PAGE); p.replace("%TOKEN%", prefs.getString("token", "")); p.replace("%NETS%", netOptions);
    String e = savedSsid; e.replace("&", "&amp;"); e.replace("<", "&lt;");
    String note = e.length() ? "<p style=\"color:#ffb000\">I can't reach \"" + e + "\". Pick your Wi-Fi below.</p>" : "";
    p.replace("%NOTE%", note); web.send(200, "text/html", p); });
  web.on("/rescan", [] { scan_networks(); web.sendHeader("Location", "/"); web.send(302); });
  web.begin(); portalOn = true;
}
void net_connect_blocking(uint32_t ms) {
  WiFi.mode(WIFI_STA); WiFi.begin(prefs.getString("ssid").c_str(), prefs.getString("pass").c_str());
  uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < ms) delay(200);
  Serial.printf("[wifi] %s \"%s\" status=%d ip=%s rssi=%d\n", WiFi.status() == WL_CONNECTED ? "connected to" : "FAILED to join",
                prefs.getString("ssid").c_str(), (int)WiFi.status(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
}
bool net_connected() { return WiFi.status() == WL_CONNECTED; }
void net_loop() {
  if (portalOn) {
    dns.processNextRequest(); web.handleClient();
    // Saved network came back (e.g. the router was still booting after a power cut): restart and run normally.
    if (savedSsid.length() && WiFi.softAPgetStationNum() == 0 && millis() - lastRetry > 30000) {
      lastRetry = millis(); WiFi.begin(savedSsid.c_str(), prefs.getString("pass").c_str());
    }
    if (savedSsid.length() && WiFi.status() == WL_CONNECTED) { Serial.println("[portal] saved Wi-Fi is back, restarting"); delay(300); ESP.restart(); }
    return;
  }
  static uint32_t last; if (WiFi.status() != WL_CONNECTED && millis() - last > 10000) { last = millis(); WiFi.reconnect(); }
}

// ---- USB setup (tools/sphere-wifi.sh). Credentials arrive over the cable, are stored in NVS, never echoed. ----
void net_serial_command(const String& line) {
  if (line == "PORTAL-TEST") {   // debug: open the setup hotspot without forgetting Wi-Fi (it reconnects and restarts)
    Serial.println("[wifi] opening setup hotspot (test)"); net_start_portal();
  } else if (line == "WIFI?") {
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
