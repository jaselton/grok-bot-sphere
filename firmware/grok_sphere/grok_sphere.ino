// Charlie, a chief-of-staff voice assistant in a Grok Bot-style sphere. Waveshare ESP32-S3-Touch-LCD-1.85C-BOX.
// Arduino-ESP32 3.3.x, LVGL 9.3.0, arduinoWebSockets (links2004), esp-sr AFE/WakeNet (bundled with arduino-esp32).
// Board: "ESP32S3 Dev Module", Flash 16MB, PSRAM "OPI PSRAM", Partition "ESP SR 16M", USB CDC on boot enabled.
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <esp_heap_caps.h>
#include "config.h"
#include "board.h"      // Display/touch/I2C/TCA9554 glue copied from Waveshare demo 01_lvgl_example (see README)
#include "ui.h"
#include "net.h"
#include "audio.h"
#include "relay_client.h"
#include "alarms.h"
#include "faces.h"
#include "ota.h"

enum class Mode { PROVISIONING, CONNECTING, IDLE, LISTENING, THINKING, SPEAKING, ERROR_ };
volatile Mode mode = Mode::IDLE;
volatile bool idlePending = false;
esp_reset_reason_t bootReason;
extern volatile uint32_t g_ui_frames;
extern uint32_t g_ws_rx_bin, g_ws_tx_bin, g_ws_rx_txt, g_ws_frag; extern bool g_ws_connected;   // 1=power-on 3=software 4=panic 5/6/7=watchdog

void setMode(Mode m) {
  mode = m;
  switch (m) {
    case Mode::IDLE:      ui_set_state(UI_IDLE);      break;
    case Mode::LISTENING: ui_set_state(UI_LISTENING); break;
    case Mode::THINKING:  ui_set_state(UI_THINKING);  break;
    case Mode::SPEAKING:  ui_set_state(UI_SPEAKING);  break;
    case Mode::PROVISIONING: ui_set_state(UI_PORTAL); break;
    default: break;
  }
}

// ---- Called from the wake-word task (audio.cpp) or on tap ----
void onWake() {
  if (mode != Mode::IDLE) { wake_resume(); return; }   // busy: ignore, but keep listening for the wake word
  setMode(Mode::LISTENING);
  relay_begin_turn();            // opens/keeps WebSocket, starts streaming mic frames
}
// ---- Called from relay_client when the relay sends {"state":"..."} ----
void onRelayState(const char* s) {
  if      (!strcmp(s, "listening")) setMode(Mode::LISTENING);
  else if (!strcmp(s, "thinking"))  { relay_end_turn(); setMode(Mode::THINKING); }   // owner stopped talking: mic off (no echo, less uplink)
  else if (!strcmp(s, "speaking"))  { relay_end_turn(); audio_stream_begin(); setMode(Mode::SPEAKING); }   // one wake = one answer
  else if (!strcmp(s, "idle"))      { audio_stream_end(); idlePending = true; }   // finish playing the reply first (see loop)
}

// ---- Alarm ringing ----
static bool alarmRinging = false;
static uint32_t ringStart = 0, nextChime = 0;
static String ringLabel;
static void alarm_stop(bool greet) {
  alarmRinging = false; ui_alarm(false, "");
  audio_flush_playback();
  if (greet) relay_greet(ringLabel);           // Charlie says good morning
}
// ---- Tap: dismiss an alarm, interrupt Charlie, or start talking ----
static void handle_tap() {
  if (alarmRinging) { alarm_stop(ringLabel != "timer" && ringLabel != "test"); return; }   // alarm: Charlie says good morning; timer: just stop
  if (mode == Mode::LISTENING || mode == Mode::THINKING || mode == Mode::SPEAKING || audio_playing()) {
    Serial.println("[tap] interrupt");
    relay_cancel(); audio_flush_playback();
    idlePending = false; setMode(Mode::IDLE); wake_resume();
    return;
  }
  onWake();
}

void setup() {
  Serial.begin(115200);
  bootReason = esp_reset_reason();             // reported in the status line (catch crash loops over USB)
  // Internal RAM is the scarce resource (Wi-Fi + TLS + ESP-SR all need it). Send ordinary mallocs above 256 bytes
  // to the 8 MB PSRAM; drivers that need internal/DMA memory ask for it explicitly with heap_caps.
  heap_caps_malloc_extmem_enable(256);
  board_init();                  // I2C, TCA9554, LCD (ST77916 QSPI), touch, backlight, LVGL tick/handler task
  ui_init();                     // eyes + clock card
  bool wipe = net_boot_counter_check();   // 3 quick power cycles -> true
  if (wipe) net_wipe_wifi();
  audio_init();                  // ES8311 + ES7210 + I2S duplex at 48 kHz, PA enable
  alarms_init();
  pinMode(PIN_BOOT, INPUT_PULLUP);   // side button = volume
  if (!net_has_credentials()) {
    setMode(Mode::PROVISIONING);
    net_start_portal();          // SoftAP + DNS + HTTP form; reboots after saving
    return;                      // loop() just services the portal
  }
  Serial.printf("[boot] connecting wifi (internal free %u, largest %u)\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  net_connect_blocking(20000);
  if (!net_connected()) {        // saved Wi-Fi unreachable (new house, changed password): open setup, keep retrying it
    Serial.println("[boot] saved Wi-Fi unreachable, opening setup hotspot");
    setMode(Mode::PROVISIONING);
    net_start_portal();
    net_boot_counter_clear_later();
    return;
  }
  configTzTime(net_tz().c_str(), "pool.ntp.org", "time.google.com");
  Serial.printf("[boot] relay_init (internal free %u, largest %u)\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  relay_init(net_relay_url(), net_device_token());
  Serial.printf("[boot] wake_begin (ESP-SR) (internal free %u, largest %u)\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#if WAKE_WORD_ENABLED
  if (!wake_begin(onWake)) Serial.println("[boot] wake word unavailable, tap to talk");
#endif
  //            // ESP-SR WakeNet, stock phrase (see README: custom "Hey Grok" needs training)
  Serial.printf("[boot] setup done (internal free %u, largest %u)\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  net_boot_counter_clear_later();// clears the quick-boot counter after BOOT_COUNT_WINDOW_MS of uptime
  setMode(Mode::IDLE);
}

void loop() {
  static String cmd;                            // USB setup commands (net_serial_command)
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { cmd.trim(); if (cmd.length()) { net_serial_command(cmd); audio_debug_command(cmd); if (cmd == "TALK") { Serial.println("[talk] simulated wake"); onWake(); }
      if (cmd == "TAP") handle_tap();
      if (cmd.startsWith("TIMER ")) { faces_timer_set(cmd.substring(6).toInt()); ui_show_face(FACE_TIMER); relay_send_timer(); }
      if (cmd.startsWith("FACE ")) ui_show_face(cmd.substring(5).toInt());
      if (cmd == "SHOT") faces_dump();
      if (cmd.startsWith("TALKLEFT ")) ui_set_talk_left(cmd.substring(9).toFloat(), 30);
      if (cmd == "RING") { alarmRinging = true; ringStart = millis(); nextChime = 0; ringLabel = "test"; ui_alarm(true, "test"); } } cmd = ""; }
    else if (cmd.length() < 300) cmd += c;
  }
  static uint32_t lastStatus;                   // status line every 5 s (boot log is lost before USB reconnects)
  if (millis() - lastStatus > 5000) {
    lastStatus = millis();
    static uint32_t lastFrames = 0; uint32_t fr = g_ui_frames; 
    Serial.printf("[status] eyes %lu/5s boot %d up %lus mode %d wifi %d relay %d tx %lu rx %lu txt %lu frag %lu heap %u psram %u\n", (unsigned long)(fr - lastFrames), (int)bootReason,
                  millis() / 1000, (int)mode, (int)WiFi.status(), (int)g_ws_connected, (unsigned long)g_ws_tx_bin, (unsigned long)g_ws_rx_bin,
                  (unsigned long)g_ws_rx_txt, (unsigned long)g_ws_frag, ESP.getFreeHeap(), ESP.getFreePsram());
    lastFrames = fr;
  }
  net_loop();                    // portal DNS/HTTP if active, Wi-Fi reconnect
  for (int i = 0; i < 4; i++) relay_loop();   // WebSocket pump: several passes so incoming audio never backs up
  static uint32_t idleSince = 0;           // safety net: the buffer holds ~32 s, so never stay "speaking" > 35 s past relay idle
  if (idlePending && !idleSince) idleSince = millis();
  if (idlePending && (!audio_playing() || millis() - idleSince > 35000)) {   // reply fully played: back to idle
    idleSince = 0;
    idlePending = false; relay_end_turn(); setMode(Mode::IDLE); wake_resume();
  }
  { // side button (BOOT, GPIO0, the UPPER button; the lower one is RESET): short = louder (wraps), long = quieter. Inside the case it is covered, so swipes on the screen do the same.
    static bool was = false; static uint32_t downAt = 0, lastEdge = 0;
    bool down = digitalRead(PIN_BOOT) == LOW;
    if (down != was && millis() - lastEdge > 30) {
      lastEdge = millis(); was = down;
      if (down) downAt = millis();
      else {
        int lvl = audio_volume_level(), n = audio_volume_levels();
        if (millis() - downAt > 600) lvl = max(0, lvl - 1); else lvl = (lvl + 1) % n;
        audio_set_volume_level(lvl, true);
      }
    }
  }
  ota_mark_good_if_due(g_ws_connected);
  if (ota_pending() && mode == Mode::IDLE && !alarmRinging && !faces_timer_running()) {   // update only when nobody's using it
    ui_set_state(UI_THINKING); ui_banner("UPDATING"); wake_pause(); relay_disconnect();
    delay(200);                  // let UPDATING reach the screen, then freeze LVGL: a display SPI interrupt during a
    lvgl_port_lock(0);           // flash write (cache off) crashes the chip, so no screen transfers while we write
    delay(60);                   // any flush already in flight finishes
    ota_run(net_relay_url(), net_device_token());   // restarts on success
    lvgl_port_unlock();
    ui_banner(""); setMode(Mode::IDLE); wake_resume();
  }
  ui_loop();                     // clock/weather refresh, idle blinking schedule
  if (ui_tapped()) handle_tap(); // tap: dismiss alarm / interrupt / talk
  if (int sw = ui_swipe()) audio_set_volume_level(audio_volume_level() + sw, true);   // swipe up/down = volume
  { String label;                // alarms: ring until tapped (auto-stop after 10 min)
    bool timerDone = faces_timer_running() && faces_timer_left_ms() <= 0;
    if (!alarmRinging && timerDone) { faces_timer_cancel(); label = "timer"; relay_send_timer(); }
    if (!alarmRinging && (timerDone || alarms_due(label))) {
      if (mode != Mode::IDLE) { relay_cancel(); audio_flush_playback(); idlePending = false; setMode(Mode::IDLE); }
      alarmRinging = true; ringStart = millis(); nextChime = 0; ringLabel = label; ui_alarm(true, label.c_str()); relay_send_alarms();
    }
    if (alarmRinging) {
      if (millis() > nextChime) { audio_chime(1); nextChime = millis() + 2200; }
      if (millis() - ringStart > 10UL * 60 * 1000) alarm_stop(false);
    }
  }
  delay(2);
}
