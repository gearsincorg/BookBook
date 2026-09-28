#include "board.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "touch.h"

static const char* TAG = "board";

namespace board {

// The button is the capacitive touch pad on GPIO6 (see touch.cpp). The BOOT button (GPIO0, active low) can be OR'd
// in as a bench fallback (CONFIG_BOOKBOOK_TOUCH_ALSO_BOOT_BUTTON), or used alone if the touch sensor is turned off.
constexpr int kLedGpio = 2;
constexpr int kLedCount = CONFIG_BOOKBOOK_LED_COUNT;
constexpr gpio_num_t kBootButton = GPIO_NUM_0;
static led_strip_handle_t s_leds;

#if !defined(CONFIG_BOOKBOOK_TOUCH_ENABLED) || defined(CONFIG_BOOKBOOK_TOUCH_ALSO_BOOT_BUTTON)
#define BOOKBOOK_USE_BOOT_BUTTON 1
#endif

esp_err_t init() {
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = kLedGpio;
    strip_cfg.max_leds = kLedCount;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_leds), TAG, "leds");
    led_strip_clear(s_leds);

#ifdef BOOKBOOK_USE_BOOT_BUTTON
    gpio_config_t btn = {};
    btn.pin_bit_mask = 1ULL << kBootButton;
    btn.mode = GPIO_MODE_INPUT;
    btn.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "button");
#endif
#ifdef CONFIG_BOOKBOOK_TOUCH_ENABLED
    ESP_RETURN_ON_ERROR(touch::start(), TAG, "touch");  // calibrates on the untouched pad: keep hands off at power-up
    ESP_LOGI(TAG, "%d LEDs on GPIO%d, button = touch pad GPIO%d%s", kLedCount, kLedGpio, CONFIG_BOOKBOOK_TOUCH_PIN,
#ifdef CONFIG_BOOKBOOK_TOUCH_ALSO_BOOT_BUTTON
             " or BOOT(GPIO0)"
#else
             ""
#endif
    );
#else
    ESP_LOGI(TAG, "%d LEDs on GPIO%d, touch sensor OFF, button = BOOT(GPIO0)", kLedCount, kLedGpio);
#endif
    return ESP_OK;
}

void set_leds(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_leds) return;
    for (int i = 0; i < kLedCount; i++) led_strip_set_pixel(s_leds, i, r, g, b);
    led_strip_refresh(s_leds);
}

int led_count() { return kLedCount; }

void set_pixel(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (s_leds && index >= 0 && index < kLedCount) led_strip_set_pixel(s_leds, index, r, g, b);
}

void show() {
    if (s_leds) led_strip_refresh(s_leds);
}

bool button_pressed() {
#ifdef BOOKBOOK_USE_BOOT_BUTTON
    if (gpio_get_level(kBootButton) == 0) return true;  // active-low: switched to GND
#endif
#ifdef CONFIG_BOOKBOOK_TOUCH_ENABLED
    return touch::present();
#else
    return false;
#endif
}

}  // namespace board
