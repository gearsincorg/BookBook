#pragma once
#include "esp_err.h"

// Capacitive touch presence sensing: the ESP32-S3's built-in touch peripheral, used as the single
// button. A port of PhilbotSays' touch_sense component (firmware/components/touch_sense/touch_sense.c)
// with the same pin (GPIO6 / XIAO D5), start-up calibration, thresholds and self-recalibration.
namespace touch {

// Starts the sensing task. It calibrates against the untouched pad first (about 100 ms), so do not
// touch the pad while the device is powering up (a high calibration recovers by itself, see touch.cpp).
esp_err_t start();

// True while the pad is touched (hysteresis- and debounce-filtered).
bool present();

}  // namespace touch
