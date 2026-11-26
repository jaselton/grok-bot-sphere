#pragma once
#include <lvgl.h>
// Watch faces drawn on a PSRAM RGB565 canvas: static dial art from face_assets.h + live hands/digits (faces.cpp).
// Carousel order (swipe left/right): eyes, dive chrono, Huey timer, Huey talk-quantity gauge, '67 stinger clock,
// Huey caution panel. The caution panel sits dark with the clock, and takes over (lit) while an alarm or timer rings.
enum FaceId { FACE_EYES = 0, FACE_DIVE, FACE_TIMER, FACE_FUEL, FACE_STINGER, FACE_CAUTION, FACE_COUNT };

void faces_init(lv_obj_t* parent);           // creates the (hidden) canvas; call with the LVGL lock held
void faces_show(int id);                     // FACE_EYES hides the canvas
void faces_tick();                           // redraw live parts when due (LVGL lock held)
void faces_set_caution(bool ring, bool timer);   // caution panel lit (ringing) or dark; legend WAKE UP or TIME UP
void faces_set_talk_left(float minutes, float limit);   // Huey talk-quantity gauge (from the relay)

// Countdown timer ("Charlie, set a timer for 12 minutes"). Runs whatever face is showing.
void     faces_timer_set(uint32_t secs);
void     faces_timer_cancel();
bool     faces_timer_running();
int32_t  faces_timer_left_ms();              // <= 0 once it has run out (while still "running")
void faces_dump();                           // debug: raw RGB565 canvas over USB (serial SHOT)
