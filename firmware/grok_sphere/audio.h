#pragma once
#include <Arduino.h>
void audio_init();
// Mic -> 16 kHz mono int16 frames (20 ms = 320 samples). Returns samples read (0 if none).
size_t audio_read_mic(int16_t* out, size_t maxSamples, uint32_t timeoutMs);
// Speaker: queue 24 kHz (SPK_RATE) mono int16 samples (ring buffer in PSRAM; i2s writer task drains it)
void   audio_play(const int16_t* in, size_t samples);
void   audio_mic_stream(bool on);       // relay turn: fill the mic ring (true) or stop (false)
void   audio_flush_playback();
void   audio_stream_begin();            // a reply is starting (jitter buffer waits for 500 ms)
void   audio_stream_end();              // the relay is done: play out what is queued
void   audio_chime(int kind);           // 0 = result ready, 1 = alarm, 2 = volume tick
int    audio_volume_level();            // 0..levels-1
int    audio_volume_levels();
void   audio_set_volume_level(int lvl, bool chime);   // saved in NVS         // barge-in: drop queued audio
bool   audio_playing();
uint8_t audio_out_level();             // 0..255 RMS of what is being played
// Wake word (ESP-SR WakeNet)
bool wake_begin(void (*onWake)());     // false if the model or memory is unavailable (tap still works)
void wake_pause();                     // while streaming to the relay
void wake_resume();
void audio_debug_command(const String& c);   // TONE | MIC | AUDIO?
