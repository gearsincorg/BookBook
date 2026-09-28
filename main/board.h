#pragma once
#include <cstdint>
#include "esp_err.h"

// Phil's VA board: a Seeed XIAO ESP32-S3 with a WS2812 LED ring and a capacitive touch pad as the one button.
// See docs/hardware.md.
namespace board {

esp_err_t init();  // LEDs and the button

void set_leds(uint8_t r, uint8_t g, uint8_t b);  // every LED the same colour, shown at once
int led_count();                                  // LEDs in the ring
void set_pixel(int index, uint8_t r, uint8_t g, uint8_t b);  // not shown until show()
void show();

// True while the button is held: the touch pad, and also the BOOT button if CONFIG_BOOKBOOK_TOUCH_ALSO_BOOT_BUTTON is
// on (or instead of the pad if CONFIG_BOOKBOOK_TOUCH_ENABLED is off).
bool button_pressed();

}  // namespace board
