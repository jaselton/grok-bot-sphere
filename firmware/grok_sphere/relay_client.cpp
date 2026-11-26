// WebSocket client to the Cloudflare Worker relay. Protocol (device <-> relay):
//   device -> relay : binary frames = PCM16 LE mono 16 kHz mic audio (20 ms = 640 bytes)
//                     text  {"t":"hello"}, {"t":"cancel"}, {"t":"greet"}, {"t":"alarms","list":[..]}
//   relay  -> device: binary frames = PCM16 LE mono 24 kHz speech (Charlie)
//                     text  {"state":"listening|thinking|speaking|idle"}, {"card":"clock"}, {"wx":"Sunny","f":72}, {"txt":"..."}
#include "relay_client.h"
#include "audio.h"
#include "ui.h"
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "alarms.h"
#include "faces.h"
#include "ota.h"
#include "config.h"

extern void onRelayState(const char*);
static WebSocketsClient ws;
static volatile bool streaming = false;
static volatile bool dropRx = false;      // after a cancel: ignore late speech until the relay says idle
static String gUrl, gToken;
uint32_t g_ws_rx_bin = 0, g_ws_tx_bin = 0, g_ws_rx_txt = 0, g_ws_frag = 0;   // debug counters (status line)
bool g_ws_connected = false;

static void onEvent(WStype_t t, uint8_t* p, size_t len) {
  switch (t) {
    case WStype_CONNECTED: g_ws_connected = true; Serial.println("[relay] connected");
      ws.sendTXT("{\"t\":\"hello\",\"fw\":\"" FW_VERSION "\"}"); relay_send_alarms(); relay_send_timer(); break;
    case WStype_BIN:       g_ws_rx_bin += len; if (!dropRx) audio_play((const int16_t*)p, len / 2); break;
    case WStype_FRAGMENT_BIN_START: case WStype_FRAGMENT: case WStype_FRAGMENT_FIN: g_ws_frag++; break;
    case WStype_TEXT: { g_ws_rx_txt++; Serial.printf("[relay] %.*s\n", (int)min<size_t>(len, 120), (const char*)p);
      JsonDocument d; if (deserializeJson(d, p, len)) break;
      if (d["state"].is<const char*>()) { if (!strcmp(d["state"], "idle")) dropRx = false; onRelayState(d["state"]); }
      if (d["chime"].is<const char*>()) audio_chime(0);
      if (d["volume"].is<JsonObject>()) {
        const char* op = d["volume"]["op"] | "";
        int lvl = audio_volume_level();
        if (!strcmp(op, "up")) lvl++; else if (!strcmp(op, "down")) lvl--; else if (!strcmp(op, "set")) lvl = (d["volume"]["level"] | 6) - 1;
        audio_set_volume_level(lvl, false);
      }
      if (d["alarm"].is<JsonObject>()) { alarms_apply(d["alarm"]["op"] | "", d["alarm"]["time"] | "", d["alarm"]["label"] | ""); relay_send_alarms(); }
      if (d["timer"].is<JsonObject>()) {               // {"timer":{"op":"set","secs":720}} / {"op":"cancel"}
        const char* op = d["timer"]["op"] | "";
        if (!strcmp(op, "set")) { faces_timer_set(d["timer"]["secs"] | 0); ui_show_face(FACE_TIMER); }
        else if (!strcmp(op, "cancel")) faces_timer_cancel();
        relay_send_timer();
      }
      if (d["ota"].is<JsonObject>()) ota_offer(d["ota"]["version"] | "", d["ota"]["size"] | 0u, d["ota"]["md5"] | "");
      if (d["usage"].is<JsonObject>()) ui_set_talk_left(d["usage"]["left"] | -1.0f, d["usage"]["limit"] | 30.0f);
      if (d["card"].is<const char*>())  ui_show_card(6000);
      if (d["wx"].is<const char*>())    ui_set_weather(d["wx"], d["f"] | 0);
      break; }
    case WStype_DISCONNECTED:
      if (g_ws_connected) { Serial.println("[relay] disconnected"); onRelayState("idle"); }   // never stay stuck mid-turn
      g_ws_connected = false; streaming = false; break;
    default: break;
  }
}
static void split_url(const String& u, String& host, uint16_t& port, String& path) {
  String s = u; s.replace("wss://", ""); int sl = s.indexOf('/'); host = s.substring(0, sl); path = s.substring(sl); port = 443;
}
void relay_init(const String& url, const String& token) {
  gUrl = url; gToken = token; String h, p; uint16_t port; split_url(url, h, port, p);
  ws.beginSSL(h.c_str(), port, (p + "?token=" + token).c_str());   // add a pinned root CA with setCACert() for production
  ws.onEvent(onEvent); ws.setReconnectInterval(3000); ws.enableHeartbeat(15000, 3000, 2);
}
void relay_begin_turn() { wake_pause(); dropRx = false; audio_mic_stream(true); streaming = true; }
void relay_cancel()     { streaming = false; audio_mic_stream(false); dropRx = true;
  bool ok = ws.isConnected() && ws.sendTXT("{\"t\":\"cancel\"}"); Serial.printf("[relay] cancel sent=%d\n", ok); }
void relay_send_alarms() { if (ws.isConnected()) { String m = "{\"t\":\"alarms\",\"list\":" + alarms_json() + "}"; ws.sendTXT(m); } }
void relay_send_timer() {     // seconds left on the countdown timer (0 = none), so Charlie can answer "how long left?"
  if (!ws.isConnected()) return;
  long left = faces_timer_running() ? max<long>(0, faces_timer_left_ms() / 1000) : 0;
  ws.sendTXT(String("{\"t\":\"timer\",\"left\":") + left + "}");
}
void relay_greet(const String& label) {
  if (!ws.isConnected()) return;
  JsonDocument d; d["t"] = "greet"; d["reason"] = "alarm"; d["label"] = label; String m; serializeJson(d, m); ws.sendTXT(m);
}
void relay_disconnect() { ws.disconnect(); g_ws_connected = false; }
void relay_end_turn()   { streaming = false; audio_mic_stream(false); }
void relay_loop() {
  ws.loop();
  if (streaming && ws.isConnected()) {
    static int16_t buf[320];
    size_t n = audio_read_mic(buf, 320, 5);
    if (n && ws.sendBIN((uint8_t*)buf, n * 2)) g_ws_tx_bin += n * 2;
  }
  ui_set_speaking_level(audio_out_level());
}
