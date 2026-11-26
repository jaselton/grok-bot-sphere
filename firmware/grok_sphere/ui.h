#pragma once
#include <lvgl.h>
enum UiState { UI_IDLE, UI_LISTENING, UI_THINKING, UI_SPEAKING, UI_PORTAL };
void ui_init();
void ui_set_state(UiState s);
void ui_loop();
bool ui_tapped();              // true once per tap (debounced)
void ui_set_speaking_level(uint8_t level0_255);   // from audio out RMS -> eye height pulse
void ui_set_weather(const char* text, int tempF);  // from relay /weather
void ui_show_card(uint32_t ms);   // clock/weather card for a few seconds
void ui_alarm(bool ringing, const char* label);   // alarm card + amber ring
void ui_show_face(int face);    // switch the idle face (FaceId in faces.h), e.g. the timer when one starts
void ui_set_talk_left(float minutes, float limit);   // Huey talk gauge
int  ui_swipe();                // +1 swipe up, -1 swipe down, 0 none (consumed)
