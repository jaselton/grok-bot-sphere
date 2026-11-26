#include "board.h"
#include "I2C_Driver.h"
#include "TCA9554PWR.h"
#include "Display_ST77916.h"
#include <Wire.h>

String g_i2c_found;   // I2C scan at boot, reported by the status line

void board_init() {
  I2C_Init();               // Wire on SDA 11 / SCL 10 @ 400 kHz
  for (uint8_t a = 1; a < 127; a++) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) { char b[6]; snprintf(b, sizeof b, " 0x%02X", a); g_i2c_found += b; } }
  TCA9554PWR_Init(0x00);    // all EXIO pins outputs (LCD reset = EXIO2, touch reset = EXIO1)
  Backlight_Init();         // PWM on GPIO 5
  LCD_Init();               // ST77916 QSPI panel, also runs Touch_Init()
  Lvgl_Init();              // lv_init, display + touch indev, LVGL task every 10 ms
}
