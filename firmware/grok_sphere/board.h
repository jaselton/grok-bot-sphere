#pragma once
// Board glue for Waveshare ESP32-S3-Touch-LCD-1.85C. Driver files are copied unmodified from
// waveshareteam/ESP32-S3-Touch-LCD-1.85C Arduino/examples (01_lvgl_example, 03_audio_out_no_tf, 08_esp_sr).
#include "LVGL_Driver.h"   // lvgl_port_lock()/lvgl_port_unlock(): recursive mutex shared with the LVGL task
void board_init();         // I2C, TCA9554, backlight, ST77916 LCD + CST816 touch, LVGL + its handler task (core 0)
#include <Arduino.h>
extern String g_i2c_found;
