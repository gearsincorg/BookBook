#pragma once
#include <cstdint>
#include "esp_err.h"

// Waveshare ESP32-S3-AUDIO-Board v1.1. See docs/hardware.md.
namespace board {

constexpr int kI2cSda = 11;
constexpr int kI2cScl = 10;
constexpr int kLedGpio = 38;
constexpr int kLedCount = 7;

enum class Key { Key1 = 9, Key2 = 10, Key3 = 11 };  // TCA9555 pin numbers
constexpr int kPaCtrlPin = 8;                        // speaker amp enable

esp_err_t init();  // I2C bus, expander, LEDs

void set_leds(uint8_t r, uint8_t g, uint8_t b);  // every LED the same colour, shown at once
int led_count();                                  // LEDs in the ring
void set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);  // not shown until show()
void show();
void set_amp(bool on);
bool key_pressed(Key key);  // reads the expander over I2C

}  // namespace board
