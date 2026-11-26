// LVGL 9.3 UI: official Grok eyes (white stadium pills on black), blink, 5 states, clock/weather card.
#include "ui.h"
#include "config.h"
#include "board.h"
#include "faces.h"
#include <Preferences.h>

struct LvLock { LvLock() { lvgl_port_lock(0); } ~LvLock() { lvgl_port_unlock(); } };
#include <math.h>
#include <time.h>

static lv_obj_t *scr, *eye[2], *ring, *lblTitle, *dots[3], *card, *lblTime, *lblDate, *lblWx, *lblPortal;
static UiState state = UI_IDLE;
static volatile bool tapped = false;
static volatile int swipe = 0;          // +1 swipe up (louder), -1 swipe down (quieter)
static uint32_t cardUntil = 0;
static int face = FACE_EYES;            // carousel position (saved), shown while idle
static bool ringing = false;            // alarm/timer ringing: caution panel overrides everything

static void lastDrawnReset();
// Which view is on screen: the caution panel while ringing, the eyes while Charlie is busy, otherwise the chosen face.
static void apply_view() {
  int v = ringing ? FACE_CAUTION : (state != UI_IDLE ? FACE_EYES : face);
  bool eyes = v == FACE_EYES;
  for (int i = 0; i < 2; i++) eyes ? lv_obj_clear_flag(eye[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(eye[i], LV_OBJ_FLAG_HIDDEN);
  if (eyes) lastDrawnReset();
  faces_show(v);
}

// ---------------- eyes ----------------
// Each eye is an lv_line with round caps: a stadium ("pill") drawn by LVGL's line renderer. Rotation is just the
// endpoint math, so there is no per-frame layer transform (the old rotated-rectangle eyes rendered too slowly for a
// 160 ms blink to show). A 30 ms LVGL timer animates blink, gaze and state targets with easing.
static lv_point_precise_t eyePts[2][2];
static float gazeX = 0, gazeY = 0, gazeTX = 0, gazeTY = 0;     // gaze offset in px (current, target)
static float lenS = 1, lenT = 1, widS = 1, widT = 1;          // per-state size (current, target)
static float blinkS = 1;                                      // 1 = open .. ~0.12 = closed
static float pulse = 1, pulseT = 1;                           // speaking loudness pulse
static uint32_t nextBlink = 0, nextGaze = 0, blinkT0 = 0;
static bool blinking = false, doubleBlink = false;
static float lastDrawn[6] = {-1};
static void lastDrawnReset() { lastDrawn[0] = -1; }
volatile uint32_t g_ui_frames = 0;                            // eye redraws (status line fps)

static float rnd(float a, float b) { return a + (b - a) * (esp_random() % 10000) / 10000.f; }
static float ease(float cur, float target, float k) { float d = target - cur; return fabsf(d) < 0.01f ? target : cur + d * k; }

static void draw_eyes() {
  const float N = SCREEN_W;
  float L = EYE_LEN_FRAC * N * lenS;                           // pill length when open
  float W = L * EYE_W_RATIO * widS;                            // pill width
  float key[6] = {L, W, blinkS, pulse, gazeX, gazeY};
  if (!memcmp(key, lastDrawn, sizeof key)) return;             // nothing changed: no redraw
  memcpy(lastDrawn, key, sizeof key);
  float dx = EYE_GAP_RATIO * EYE_LEN_FRAC * N * cosf(EYE_LINE_DEG * M_PI / 180.f) / 2.f;   // spacing fixed by the idle size
  float dy = EYE_GAP_RATIO * EYE_LEN_FRAC * N * sinf(EYE_LINE_DEG * M_PI / 180.f) / 2.f;
  float cx = N / 2 + 0.09f * N + gazeX, cy = N / 2 - 0.10f * N + gazeY;   // slightly right/up like the official mark
  float ex[2] = {cx - dx, cx + dx}, ey[2] = {cy + dy, cy - dy};          // eye line rises to the right
  float total = fmaxf(L * blinkS * pulse, 6.f);                // blink squashes along the long axis
  float w = fminf(W, total);
  float half = fmaxf(0.f, (total - w) / 2.f);                  // segment half-length; round caps add w/2 each end
  float a = EYE_TILT_DEG * M_PI / 180.f, sx = sinf(a), cy_ = cosf(a);    // top of the pill leans LEFT
  for (int i = 0; i < 2; i++) {
    eyePts[i][0] = { (lv_value_precise_t)(ex[i] - sx * half), (lv_value_precise_t)(ey[i] - cy_ * half) };
    eyePts[i][1] = { (lv_value_precise_t)(ex[i] + sx * half), (lv_value_precise_t)(ey[i] + cy_ * half) };
    lv_obj_set_style_line_width(eye[i], (int32_t)w, 0);
    lv_line_set_points(eye[i], eyePts[i], 2);
  }
  g_ui_frames++;
}

static void eyes_tick(lv_timer_t*) {                           // runs in the LVGL task (lock held)
  uint32_t now = millis();
  // blink: close 90 ms, hold 40 ms, open 130 ms; every 2.5-6 s while idle/listening; 20% double blinks
  if (!blinking && (state == UI_IDLE || state == UI_LISTENING || state == UI_PORTAL) && now > nextBlink) {
    blinking = true; blinkT0 = now; doubleBlink = (esp_random() % 5) == 0;
  }
  if (blinking) {
    uint32_t t = now - blinkT0;
    if (t < 90) blinkS = 1 - 0.88f * t / 90.f;
    else if (t < 130) blinkS = 0.12f;
    else if (t < 260) blinkS = 0.12f + 0.88f * (t - 130) / 130.f;
    else {
      blinkS = 1; blinking = false;
      if (doubleBlink) { doubleBlink = false; nextBlink = now + 120; }
      else nextBlink = now + (uint32_t)rnd(2500, 6000);
    }
  }
  // gaze: idle eyes wander; listening looks at you; thinking looks up and to the left
  if (state == UI_IDLE || state == UI_PORTAL) {
    if (now > nextGaze) {
      if (esp_random() % 3 == 0) { gazeTX = 0; gazeTY = 0; }  // often come back to centre
      else { gazeTX = rnd(-26, 26); gazeTY = rnd(-16, 18); }
      nextGaze = now + (uint32_t)rnd(1200, 4000);
    }
  } else if (state == UI_LISTENING) { gazeTX = 0; gazeTY = 4; }
  else if (state == UI_THINKING)    { gazeTX = -22; gazeTY = -20; }
  else if (state == UI_SPEAKING)    { gazeTX = 0; gazeTY = 0; }
  gazeX = ease(gazeX, gazeTX, 0.22f); gazeY = ease(gazeY, gazeTY, 0.22f);
  lenS = ease(lenS, lenT, 0.25f); widS = ease(widS, widT, 0.25f);
  pulse = ease(pulse, state == UI_SPEAKING ? pulseT : 1.f, 0.5f);
  if (state == UI_THINKING) { int k = (now / 300) % 3; for (int i = 0; i < 3; i++) lv_obj_set_style_bg_opa(dots[i], i <= k ? LV_OPA_COVER : LV_OPA_30, 0); }
  draw_eyes();
}
static void on_tap(lv_event_t*) { tapped = true; }
static void on_gesture(lv_event_t*) {   // swipe up/down on the screen = volume (the side button is inside the case)
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
  if (d == LV_DIR_TOP) swipe = +1; else if (d == LV_DIR_BOTTOM) swipe = -1;
  else if ((d == LV_DIR_LEFT || d == LV_DIR_RIGHT) && state == UI_IDLE && !ringing) {   // left/right = next/previous face
    face = (face + (d == LV_DIR_LEFT ? 1 : FACE_COUNT - 1)) % FACE_COUNT;
    Preferences p; p.begin("ui", false); p.putUChar("face", face); p.end();
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN); cardUntil = 0;
    apply_view();
    lv_indev_wait_release(lv_indev_active());
    return;
  }
  if (swipe) lv_indev_wait_release(lv_indev_active());   // don't also count it as a tap
}

void ui_init() {
  LvLock lk;
  scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr, on_tap, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, NULL);

  ring = lv_obj_create(scr);                       // neon ring for LISTENING
  lv_obj_set_size(ring, SCREEN_W - 6, SCREEN_H - 6); lv_obj_center(ring);
  lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(ring, lv_color_hex(0x39FF14), 0);
  lv_obj_set_style_border_width(ring, 7, 0);
  lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);

  for (int i = 0; i < 2; i++) {                    // eyes: full-screen line objects, absolute point coordinates
    eye[i] = lv_line_create(scr);
    lv_obj_set_pos(eye[i], 0, 0); lv_obj_set_size(eye[i], SCREEN_W, SCREEN_H);
    lv_obj_set_style_line_color(eye[i], lv_color_white(), 0);
    lv_obj_set_style_line_rounded(eye[i], true, 0);
    lv_obj_clear_flag(eye[i], (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
  }
  draw_eyes();
  nextBlink = millis() + 1500;
  lv_timer_create(eyes_tick, 30, NULL);

  for (int i = 0; i < 3; i++) {                    // THINKING dots
    dots[i] = lv_obj_create(scr); lv_obj_set_size(dots[i], 16, 16);
    lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dots[i], lv_color_hex(0x39FF14), 0);
    lv_obj_set_style_border_width(dots[i], 0, 0);
    lv_obj_align(dots[i], LV_ALIGN_BOTTOM_MID, (i - 1) * 34, -52);
    lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
  }
  faces_init(scr);                                 // watch faces canvas (above the eyes, below the card)
  { Preferences p; p.begin("ui", true); face = p.getUChar("face", FACE_EYES) % FACE_COUNT; p.end(); }

  lblTitle = lv_label_create(scr);                 // small status pill text under the eyes
  lv_obj_set_style_text_color(lblTitle, lv_color_hex(0x39FF14), 0);
  lv_obj_set_style_text_font(lblTitle, &lv_font_montserrat_20, 0);
  lv_obj_align(lblTitle, LV_ALIGN_BOTTOM_MID, 0, -50); lv_label_set_text(lblTitle, "");

  // clock / weather card (shown on tap when idle, or every few minutes)
  card = lv_obj_create(scr); lv_obj_set_size(card, SCREEN_W, SCREEN_H);
  lv_obj_set_style_bg_color(card, lv_color_black(), 0); lv_obj_set_style_border_width(card, 0, 0);
  lblTime = lv_label_create(card); lv_obj_set_style_text_font(lblTime, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(lblTime, lv_color_white(), 0); lv_obj_align(lblTime, LV_ALIGN_CENTER, 0, -30);
  lblDate = lv_label_create(card); lv_obj_set_style_text_color(lblDate, lv_color_hex(0x39FF14), 0);
  lv_obj_align(lblDate, LV_ALIGN_CENTER, 0, 25);
  lblWx = lv_label_create(card); lv_obj_set_style_text_color(lblWx, lv_color_white(), 0);
  lv_obj_align(lblWx, LV_ALIGN_CENTER, 0, 65);
  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

  lblPortal = lv_label_create(scr); lv_obj_set_style_text_color(lblPortal, lv_color_white(), 0);
  lv_obj_set_style_text_align(lblPortal, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lblPortal, LV_ALIGN_BOTTOM_MID, 0, -40); lv_label_set_text(lblPortal, "");
}

void ui_set_state(UiState s) {
  LvLock lk;
  state = s;
  bool think = (s == UI_THINKING);
  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
  s == UI_LISTENING ? lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < 3; i++) think ? lv_obj_clear_flag(dots[i], LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(dots[i], LV_OBJ_FLAG_HIDDEN);
  lenT = think ? 0.62f : s == UI_LISTENING ? 1.08f : s == UI_SPEAKING ? 0.92f : 1.0f;   // squint while thinking, wide while listening
  widT = s == UI_LISTENING ? 1.06f : 1.0f;
  if (s != UI_IDLE) blinking = false, blinkS = 1;
  lv_label_set_text(lblTitle, s == UI_LISTENING ? "LISTENING" : s == UI_THINKING ? "" : s == UI_SPEAKING ? "CHARLIE" : "");
  if (s == UI_PORTAL) {   // same SSID that net_start_portal() broadcasts
    String id = String((uint32_t)(ESP.getEfuseMac() >> 24), HEX); id.toUpperCase();
    lv_label_set_text_fmt(lblPortal, "Join Wi-Fi\n%s%s\nto set me up", PORTAL_SSID_PREFIX, id.substring(0, 4).c_str());
  }
  else lv_label_set_text(lblPortal, "");
  apply_view();
}

void ui_set_speaking_level(uint8_t lvl) {
  LvLock lk;            // pulse eye height with output loudness
  pulseT = 1.0f - (lvl * 0.28f) / 255.f;       // louder speech -> eyes squash a little (eased in eyes_tick)
}
void ui_set_weather(const char* t, int f) { LvLock lk; lv_label_set_text_fmt(lblWx, "%s  %d\xC2\xB0F", t, f); }

bool ui_tapped() { bool t = tapped; tapped = false; return t; }
int ui_swipe() { int v = swipe; swipe = 0; return v; }

void ui_loop() {
  LvLock lk;
  uint32_t now = millis();
  static uint32_t lastClock = 0;                   // clock labels once a second (was every loop pass)
  struct tm t; if (now - lastClock > 1000 && getLocalTime(&t, 0)) {
    lastClock = now;
    char b[16]; strftime(b, sizeof b, "%-I:%M", &t); lv_label_set_text(lblTime, b);
    strftime(b, sizeof b, "%a %b %-d", &t); lv_label_set_text(lblDate, b);
  }
  if (cardUntil && now > cardUntil) { cardUntil = 0; lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN); }
  faces_tick();
}

void ui_show_card(uint32_t ms) { LvLock lk; lv_obj_clear_flag(card, LV_OBJ_FLAG_HIDDEN); cardUntil = millis() + ms; }

// Alarm or timer ringing: the Huey caution panel (WAKE UP / TIME UP, flashing MASTER CAUTION). Tap = press to reset.
void ui_alarm(bool ring, const char* label) {
  LvLock lk;
  ringing = ring;
  faces_set_caution(ring, label && !strcmp(label, "timer"));
  if (ring) { lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN); cardUntil = 0; }
  apply_view();
}
void ui_show_face(int f) { LvLock lk; if (f >= 0 && f < FACE_COUNT) { face = f; apply_view(); } }
void ui_set_talk_left(float minutes, float limit) { LvLock lk; faces_set_talk_left(minutes, limit); }

void ui_banner(const char* text) { LvLock lk; lv_label_set_text(lblTitle, text); }
