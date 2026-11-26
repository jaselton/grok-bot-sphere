// Watch faces. The dial art is baked by tools/make_face_assets.py (I8 + palette); each redraw decodes it into the
// canvas and draws the live parts with small anti-aliased primitives (convex polygons, discs, an arc band, A8 text).
// Geometry mirrors tools/render_watch_dial.py and tools/render_faces.py (same numbers, 1x pixels, centre 180,180).
#include "faces.h"
#include "face_assets.h"
#include <Arduino.h>
#include <math.h>
#include <time.h>
#include <esp_heap_caps.h>

static const int W = 360;
static const float CX = 180, CY = 180;
static lv_obj_t* canvas;
static uint16_t* fb;                 // 360x360 RGB565 (lv_color16_t layout)
static int shown = FACE_EYES;
static bool dirty = true, cautionTimer = false, cautionRing = false;
static uint32_t lastKey = 0xFFFFFFFF;
static float talkLeft = -1, talkLimit = 30;
static uint32_t cdEnd = 0; static bool timerOn = false;

// ---------------------------------------------------------------- pixel helpers
static inline uint16_t c565(uint8_t r, uint8_t g, uint8_t b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }
static inline void blend(int x, int y, uint16_t c, float a) {
  if ((unsigned)x >= (unsigned)W || (unsigned)y >= (unsigned)W || a <= 0.004f) return;
  uint16_t& d = fb[y * W + x];
  if (a >= 0.996f) { d = c; return; }
  uint32_t k = (uint32_t)(a * 256), ik = 256 - k;
  uint32_t r = (((c >> 11) & 31) * k + ((d >> 11) & 31) * ik) >> 8;
  uint32_t g = (((c >> 5) & 63) * k + ((d >> 5) & 63) * ik) >> 8;
  uint32_t b = ((c & 31) * k + (d & 31) * ik) >> 8;
  d = (r << 11) | (g << 5) | b;
}
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

struct P { float x, y; };
// Anti-aliased convex polygon: coverage from the distance to the nearest edge. grow > 0 fattens it (outlines).
static void poly(const P* p, int n, uint16_t c, float grow = 0) {
  float area = 0, x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
  for (int i = 0; i < n; i++) { const P &a = p[i], &b = p[(i + 1) % n]; area += a.x * b.y - b.x * a.y;
    x0 = fminf(x0, a.x); y0 = fminf(y0, a.y); x1 = fmaxf(x1, a.x); y1 = fmaxf(y1, a.y); }
  float s = area > 0 ? 1 : -1;
  float nx[8], ny[8], nd[8];
  for (int i = 0; i < n; i++) {
    const P &a = p[i], &b = p[(i + 1) % n]; float ex = b.x - a.x, ey = b.y - a.y, L = sqrtf(ex * ex + ey * ey);
    if (L < 1e-4f) { nx[i] = ny[i] = 0; nd[i] = 1e9; continue; }
    nx[i] = -ey / L * s; ny[i] = ex / L * s; nd[i] = -(nx[i] * a.x + ny[i] * a.y);   // inside: n.p + d > 0
  }
  int ix0 = (int)floorf(x0 - grow - 1), iy0 = (int)floorf(y0 - grow - 1), ix1 = (int)ceilf(x1 + grow + 1), iy1 = (int)ceilf(y1 + grow + 1);
  for (int y = iy0; y <= iy1; y++) for (int x = ix0; x <= ix1; x++) {
    float px = x + 0.5f, py = y + 0.5f, m = 1e9;
    for (int i = 0; i < n; i++) { float d = nx[i] * px + ny[i] * py + nd[i]; if (d < m) m = d; if (m < -grow - 1) break; }
    blend(x, y, c, clamp01(m + grow + 0.5f));
  }
}
static void line(float ax, float ay, float bx, float by, float w, uint16_t c) {
  float dx = bx - ax, dy = by - ay, L = sqrtf(dx * dx + dy * dy); if (L < 1e-3f) return;
  float px = -dy / L * w / 2, py = dx / L * w / 2;
  P q[4] = {{ax + px, ay + py}, {bx + px, by + py}, {bx - px, by - py}, {ax - px, ay - py}}; poly(q, 4, c);
}
static void disc(float cx, float cy, float r, uint16_t c) {
  for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
    float dx = x + 0.5f - cx, dy = y + 0.5f - cy; blend(x, y, c, clamp01(r - sqrtf(dx * dx + dy * dy) + 0.5f));
  }
}
static void rrect(float x0, float y0, float x1, float y1, float r, uint16_t c) {
  float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, hx = (x1 - x0) / 2 - r, hy = (y1 - y0) / 2 - r;
  for (int y = (int)y0 - 1; y <= (int)y1 + 1; y++) for (int x = (int)x0 - 1; x <= (int)x1 + 1; x++) {
    float qx = fabsf(x + 0.5f - cx) - hx, qy = fabsf(y + 0.5f - cy) - hy;
    float ox = fmaxf(qx, 0), oy = fmaxf(qy, 0);
    float d = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0) - r;
    blend(x, y, c, clamp01(0.5f - d));
  }
}
// Band between radii r0..r1 from 12 o'clock clockwise to deg.
static void arc_band(float r0, float r1, float deg, uint16_t c) {
  if (deg <= 0) return;
  float ro = r1 + 1, ri = r0 - 1;
  for (int y = (int)(CY - ro); y <= (int)(CY + ro); y++) {
    float dy = y + 0.5f - CY; if (fabsf(dy) > ro) continue;
    float xo = sqrtf(ro * ro - dy * dy), xi = fabsf(dy) < ri ? sqrtf(ri * ri - dy * dy) : 0;   // only the ring's pixels
    for (int side = -1; side <= 1; side += 2) {
      int xa = (int)floorf(side < 0 ? CX - xo : CX + xi), xb = (int)ceilf(side < 0 ? CX - xi : CX + xo);
      for (int x = xa; x <= xb; x++) {
        float dx = x + 0.5f - CX, rr = sqrtf(dx * dx + dy * dy);
        if (rr < ri || rr > ro) continue;
        float a = atan2f(dx, -dy) * 180.f / M_PI; if (a < 0) a += 360;
        if (a > deg) continue;
        blend(x, y, c, clamp01(fminf(rr - r0, r1 - rr) + 0.5f));
      }
    }
  }
}
static int glyph_index(char ch) { if (ch >= '0' && ch <= '9') return ch - '0'; return ch == ':' ? 10 : ch == '.' ? 11 : -1; }
static void text(const FaceFont& f, const char* s, float cx, float cy, uint16_t c) {
  int w = 0; for (const char* q = s; *q; q++) { int g = glyph_index(*q); if (g >= 0) w += f.g[g].w; }
  int x = (int)lroundf(cx - w / 2.f), y0 = (int)lroundf(cy - f.h / 2.f);
  for (const char* q = s; *q; q++) {
    int g = glyph_index(*q); if (g < 0) continue;
    const uint8_t* a = f.a8 + f.g[g].off; int gw = f.g[g].w;
    for (int yy = 0; yy < f.h; yy++) for (int xx = 0; xx < gw; xx++) { uint8_t v = a[yy * gw + xx]; if (v) blend(x + xx, y0 + yy, c, v / 255.f); }
    x += gw;
  }
}
static void sprite(const FaceSprite& s, float cx, float cy, uint16_t c) {
  int x0 = (int)lroundf(cx - s.w / 2.f), y0 = (int)lroundf(cy - s.h / 2.f);
  for (int y = 0; y < s.h; y++) for (int x = 0; x < s.w; x++) { uint8_t v = s.a8[y * s.w + x]; if (v) blend(x0 + x, y0 + y, c, v / 255.f); }
}
static void decode(const uint16_t* pal, const uint8_t* idx) { for (int i = 0; i < W * W; i++) fb[i] = pal[idx[i]]; }

// Unit vector for an angle in degrees clockwise from 12 o'clock.
static void dir(float deg, float& ux, float& uy) { float a = deg * M_PI / 180.f; ux = sinf(a); uy = -cosf(a); }

// ---------------------------------------------------------------- hands
static void sword(float deg, float len, float width, float tail, uint16_t body, uint16_t lume) {   // dive watch hands
  float ux, uy; dir(deg, ux, uy); float px = -uy, py = ux, Wd = width / 2;
  float shx = CX + ux * len * 0.80f, shy = CY + uy * len * 0.80f;
  P o[5] = {{CX - ux * tail + px * Wd * 0.5f, CY - uy * tail + py * Wd * 0.5f}, {shx + px * Wd, shy + py * Wd},
            {CX + ux * len, CY + uy * len}, {shx - px * Wd, shy - py * Wd}, {CX - ux * tail - px * Wd * 0.5f, CY - uy * tail - py * Wd * 0.5f}};
  poly(o, 5, body);
  P i[5] = {{CX + ux * len * 0.22f + px * Wd * 0.45f, CY + uy * len * 0.22f + py * Wd * 0.45f}, {shx + px * Wd * 0.62f, shy + py * Wd * 0.62f},
            {CX + ux * len * 0.93f, CY + uy * len * 0.93f}, {shx - px * Wd * 0.62f, shy - py * Wd * 0.62f},
            {CX + ux * len * 0.22f - px * Wd * 0.45f, CY + uy * len * 0.22f - py * Wd * 0.45f}};
  poly(i, 5, lume);
}
static void needle(float deg, float len, float tail, float w) {   // Huey instrument needle + cap
  float ux, uy; dir(deg, ux, uy); float px = -uy, py = ux, Wd = w / 2;
  P n[5] = {{CX - ux * tail + px * Wd, CY - uy * tail + py * Wd}, {CX + ux * len * 0.86f + px * Wd * 0.7f, CY + uy * len * 0.86f + py * Wd * 0.7f},
            {CX + ux * len, CY + uy * len}, {CX + ux * len * 0.86f - px * Wd * 0.7f, CY + uy * len * 0.86f - py * Wd * 0.7f},
            {CX - ux * tail - px * Wd, CY - uy * tail - py * Wd}};
  poly(n, 5, c565(236, 236, 230));
  disc(CX, CY, 13, c565(90, 90, 94)); disc(CX, CY, 12, c565(30, 30, 32)); disc(CX, CY, 3, c565(120, 120, 124));
}
static void baton(float deg, float len, float w, float tail) {    // stinger clock hands: chrome with a dark edge
  float ux, uy; dir(deg, ux, uy); float px = -uy, py = ux, Wd = w / 2;
  P h[4] = {{CX - ux * tail + px * Wd, CY - uy * tail + py * Wd}, {CX + ux * len + px * Wd * 0.35f, CY + uy * len + py * Wd * 0.35f},
            {CX + ux * len - px * Wd * 0.35f, CY + uy * len - py * Wd * 0.35f}, {CX - ux * tail - px * Wd, CY - uy * tail - py * Wd}};
  poly(h, 4, c565(30, 30, 32), 1.0f); poly(h, 4, c565(214, 214, 220));
}

// ---------------------------------------------------------------- faces
static bool now_tm(struct tm& t) { return getLocalTime(&t, 0); }

static void draw_dive(const struct tm& t) {
  decode(face_dive_pal, face_dive_idx);
  char b[4]; snprintf(b, sizeof b, "%d", t.tm_mday); text(font_date, b, 180, 268.5f, c565(10, 10, 10));
  float ux, uy;
  dir(t.tm_sec * 6.f, ux, uy); line(118, 180, 118 + ux * 28, 180 + uy * 28, 2.2f, c565(242, 242, 240)); disc(118, 180, 3, c565(242, 242, 240));
  dir(t.tm_hour * 15.f + t.tm_min * 0.25f, ux, uy); line(242, 180, 242 + ux * 28, 180 + uy * 28, 2.2f, c565(255, 94, 0)); disc(242, 180, 3, c565(255, 94, 0));
  uint16_t steel = c565(200, 202, 206), lume = c565(236, 236, 228);
  sword((t.tm_hour % 12) * 30.f + t.tm_min * 0.5f, 82, 15, 14, steel, lume);
  sword(t.tm_min * 6.f + t.tm_sec * 0.1f, 124, 12, 16, steel, lume);
  dir(t.tm_sec * 6.f, ux, uy);
  line(CX - ux * 30, CY - uy * 30, CX + ux * 140, CY + uy * 140, 2.0f, c565(242, 242, 240));
  line(CX + ux * 118, CY + uy * 118, CX + ux * 140, CY + uy * 140, 2.4f, c565(255, 94, 0));
  disc(CX, CY, 6, steel); disc(CX, CY, 2.5f, c565(40, 40, 42));
}
static void draw_timer() {
  decode(face_timer_pal, face_timer_idx);
  int32_t left = timerOn ? max<int32_t>(0, faces_timer_left_ms()) : 0;
  uint32_t s = (left + 999) / 1000;
  float remMin = left / 60000.f;
  arc_band(149, 152, fminf(remMin, 60.f) * 6.f, c565(255, 176, 0));
  char b[12];
  if (s >= 3600) snprintf(b, sizeof b, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
  else snprintf(b, sizeof b, "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  text(font_timer, b, 180, 226, timerOn ? c565(255, 176, 0) : c565(90, 90, 94));
  needle(fmodf(remMin, 60.f) * 6.f, 146, 20, 6);
}
static void draw_fuel() {
  decode(face_fuel_pal, face_fuel_idx);
  float v = talkLeft < 0 ? talkLimit : talkLeft;
  float dial = fminf(fmaxf(v * 30.f / fmaxf(talkLimit, 1), 0), 30);   // dial is 0-30; scale if the limit differs
  char b[8]; snprintf(b, sizeof b, "%.1f", v); text(font_fuel, b, 180, 238, c565(236, 236, 230));
  if (talkLeft >= 0 && talkLeft <= 5) { rrect(136, 264, 224, 282, 3, c565(255, 176, 0)); sprite(spr_lowtalk, 180, 273, c565(20, 16, 0)); }
  needle(-135 + 270 * dial / 30.f, 146, 26, 7);
}
static void draw_caution(const struct tm* t, bool lit) {
  decode(face_caution_pal, face_caution_idx);
  uint16_t amber = c565(255, 176, 0), ink = c565(20, 16, 0);
  if (cautionRing) { rrect(140, 76, 220, 104, 0.5f, amber); sprite(cautionTimer ? spr_timeup : spr_wake, 180, 90, ink); }
  if (cautionRing && lit) { rrect(112, 160, 248, 208, 4, amber); sprite(spr_master, 180, 176, ink); sprite(spr_caution, 180, 195, ink); }
  if (t) { char b[8]; int h = t->tm_hour % 12; snprintf(b, sizeof b, "%d:%02d", h ? h : 12, t->tm_min); text(font_caution, b, 180, 260, c565(236, 236, 230)); }
}
static void draw_stinger(const struct tm& t) {
  decode(face_stinger_pal, face_stinger_idx);
  baton((t.tm_hour % 12) * 30.f + t.tm_min * 0.5f, 88, 10, 18);
  baton(t.tm_min * 6.f + t.tm_sec * 0.1f, 132, 8, 18);
  float ux, uy; dir(t.tm_sec * 6.f, ux, uy);
  line(CX - ux * 26, CY - uy * 26, CX + ux * 146, CY + uy * 146, 2.4f, c565(220, 30, 24));
  disc(CX, CY, 10, c565(90, 90, 94)); disc(CX, CY, 9, c565(210, 210, 214));
}

// ---------------------------------------------------------------- API
void faces_init(lv_obj_t* parent) {
  fb = (uint16_t*)heap_caps_malloc(W * W * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!fb) { Serial.println("[faces] no PSRAM for the canvas"); return; }
  memset(fb, 0, W * W * 2);
  canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(canvas, fb, W, W, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(canvas, 0, 0);
  lv_obj_clear_flag(canvas, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_add_flag(canvas, (lv_obj_flag_t)(LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
}
void faces_show(int id) {
  if (!canvas) return;
  if (id == shown) return;
  shown = id; dirty = true; lastKey = 0xFFFFFFFF;
  if (id == FACE_EYES) lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
  else { faces_tick(); lv_obj_clear_flag(canvas, LV_OBJ_FLAG_HIDDEN); }
}
void faces_tick() {
  if (!canvas || shown == FACE_EYES) return;
  struct tm t; bool haveT = now_tm(t);
  uint32_t ms = millis(), key;
  switch (shown) {                                  // redraw only when what's on the face changes
    case FACE_TIMER:   key = timerOn ? (uint32_t)(max<int32_t>(0, faces_timer_left_ms() + 999) / 1000) : 0xFFFF0000; break;
    case FACE_FUEL:    key = (uint32_t)(talkLeft * 10) ^ 0x5A5A0000; break;
    case FACE_CAUTION: key = (cautionRing && ms % 1000 < 600) | ((haveT ? t.tm_min : 0) << 1) | (cautionTimer << 8) | (cautionRing << 9); break;
    default:           key = haveT ? (uint32_t)(t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec) : 0; break;
  }
  if (!dirty && key == lastKey) return;
  lastKey = key; dirty = false;
  struct tm z = {}; if (!haveT) { z.tm_hour = 10; z.tm_min = 9; z.tm_mday = 1; }
  const struct tm& tt = haveT ? t : z;
  uint32_t t0 = micros();
  switch (shown) {
    case FACE_DIVE:    draw_dive(tt); break;
    case FACE_TIMER:   draw_timer(); break;
    case FACE_FUEL:    draw_fuel(); break;
    case FACE_CAUTION: draw_caution(haveT ? &t : nullptr, ms % 1000 < 600); break;
    case FACE_STINGER: draw_stinger(tt); break;
  }
  lv_obj_invalidate(canvas);
  static uint32_t worst = 0; uint32_t us = micros() - t0;
  if (us > worst) { worst = us; Serial.printf("[faces] face %d drawn in %lu us\n", shown, (unsigned long)us); }
}
void faces_set_caution(bool ring, bool timer) { cautionRing = ring; if (ring) cautionTimer = timer; dirty = true; }
void faces_set_talk_left(float minutes, float limit) { talkLeft = minutes; talkLimit = limit > 0 ? limit : 30; dirty = true; }

void faces_timer_set(uint32_t secs) { cdEnd = millis() + secs * 1000UL; timerOn = secs > 0; dirty = true; }
void faces_timer_cancel() { timerOn = false; dirty = true; }
bool faces_timer_running() { return timerOn; }
int32_t faces_timer_left_ms() { return timerOn ? (int32_t)(cdEnd - millis()) : 0; }

// Debug: dump the canvas over USB (serial command SHOT) so the faces can be checked without a camera.
void faces_dump() {
  if (!fb) return;
  Serial.printf("SHOT %d %d\n", W, W); Serial.flush();
  const uint8_t* p = (const uint8_t*)fb; size_t n = W * W * 2;
  for (size_t off = 0; off < n; off += 4096) { Serial.write(p + off, min<size_t>(4096, n - off)); Serial.flush(); }
  Serial.println("\nSHOT END");
}
