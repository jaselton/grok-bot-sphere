#pragma once
// Waveshare ESP32-S3-Touch-LCD-1.85C **V2** (check silkscreen "Rev2.0"). V1 has different audio pins!
// Pins from Waveshare wiki (waveshare.net/wiki/ESP32-S3-Touch-LCD-1.85C_V2) + lcdwizard.com cross-check.
#define PIN_LCD_SCK 40
#define PIN_LCD_CS  21
#define PIN_LCD_D0  46
#define PIN_LCD_D1  45
#define PIN_LCD_D2  42
#define PIN_LCD_D3  41
#define PIN_LCD_TE  18
#define PIN_LCD_BL   5      // PWM backlight
// LCD reset = TCA9554 EXIO2, touch reset = EXIO1, SD CS = EXIO3, RTC int = EXIO4 (TCA9554 @0x20)
#define PIN_I2C_SDA 11      // shared: touch CST816 @0x15, TCA9554 @0x20, RTC PCF85063 @0x51, ES8311, ES7210
#define PIN_I2C_SCL 10
#define PIN_TP_INT   4
#define PIN_BOOT     0
// Audio: ES8311 (DAC) + ES7210 (dual mic ADC) share one I2S bus
#define PIN_I2S_MCLK 2
#define PIN_I2S_BCK 48
#define PIN_I2S_LRCK 38
#define PIN_I2S_DIN 39      // mic -> ESP
#define PIN_I2S_DOUT 47     // ESP -> speaker
#define PIN_PA_EN   15      // NS4150B amp enable (HIGH = on)

#define SCREEN_W 360
#define SCREEN_H 360
#define CODEC_RATE 48000    // ES8311/ES7210 + I2S run at 48 kHz stereo
#define MIC_RATE   16000    // mic after 3:1 decimation (wake word + relay uplink)
#define SPK_RATE   24000    // speech from the relay (xAI output rate); upsampled 2x to CODEC_RATE

// ---- Official Grok Bot eye geometry (from sphere/README, measured on @bot profile image) ----
#define EYE_LEN_FRAC   0.31f   // pill length / screen diameter (idle)
#define EYE_W_RATIO    0.47f   // pill width / length
#define EYE_GAP_RATIO  0.95f   // centre distance / length
#define EYE_TILT_DEG   35.0f   // major axis lean from vertical, top toward the LEFT
#define EYE_LINE_DEG   25.0f   // eye line rises to the right

// ---- Device config (stored in NVS, set via captive portal) ----
#define FW_VERSION "0.6"          // reported in hello; the relay offers OTA images newer than this
#define PORTAL_SSID_PREFIX "Charlie-"
#define BOOT_COUNT_RESET   3       // 3 quick power cycles => wipe Wi-Fi + reopen portal
#define BOOT_COUNT_WINDOW_MS 8000  // a boot counts as "quick" if we are powered off again within this

// Wake word (ESP-SR WakeNet "Hi ESP"). Needs ~100+ KB internal RAM next to Wi-Fi + TLS. Tap-to-talk always works.
#ifndef WAKE_WORD_ENABLED
#define WAKE_WORD_ENABLED 1   // esp-sr AFE + WakeNet only, driven directly in audio.cpp (Arduino ESP_SR wrapper crashed)
#endif
