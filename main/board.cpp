#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "led_strip.h"
#include "sdkconfig.h"
#include "touch.h"

static const char* TAG = "board";

namespace board {

#if CONFIG_BOOKBOOK_BOARD_XIAO_ESP32S3

// Temporary stand-in on the PhilbotSays PCB: an external WS2812-style LED string on GPIO2, and Key1 is
// the capacitive touch pad on GPIO6 (see touch.cpp). The BOOT button (GPIO0, active low) can be OR'd in
// as a bench fallback (CONFIG_BOOKBOOK_TOUCH_ALSO_BOOT_BUTTON). No amp, no expander.
constexpr int kXiaoLedGpio = 2;
constexpr int kXiaoLedCount = CONFIG_BOOKBOOK_XIAO_LED_COUNT;
constexpr gpio_num_t kXiaoButton = GPIO_NUM_0;
static led_strip_handle_t s_leds;

esp_err_t init() {
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = kXiaoLedGpio;
    strip_cfg.max_leds = kXiaoLedCount;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_leds), TAG, "leds");
    led_strip_clear(s_leds);

#if !defined(CONFIG_BOOKBOOK_TOUCH_ENABLED) || defined(CONFIG_BOOKBOOK_TOUCH_ALSO_BOOT_BUTTON)
#define BOOKBOOK_USE_BOOT_BUTTON 1
#endif
#ifdef BOOKBOOK_USE_BOOT_BUTTON
    gpio_config_t btn = {};
    btn.pin_bit_mask = 1ULL << kXiaoButton;
    btn.mode = GPIO_MODE_INPUT;
    btn.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&btn), TAG, "button");
#endif
#ifdef CONFIG_BOOKBOOK_TOUCH_ENABLED
    ESP_RETURN_ON_ERROR(touch::start(), TAG, "touch");  // calibrates on the untouched pad: keep hands off at power-up
    ESP_LOGI(TAG, "XIAO ESP32-S3 dev board: %d LEDs on GPIO%d, Key1 = touch pad GPIO%d%s", kXiaoLedCount, kXiaoLedGpio,
             CONFIG_BOOKBOOK_TOUCH_PIN,
#ifdef CONFIG_BOOKBOOK_TOUCH_ALSO_BOOT_BUTTON
             " or BOOT(GPIO0)"
#else
             ""
#endif
    );
#else
    ESP_LOGI(TAG, "XIAO ESP32-S3 dev board: %d LEDs on GPIO%d, touch sensor OFF, Key1 = BOOT(GPIO0)", kXiaoLedCount, kXiaoLedGpio);
#endif
    return ESP_OK;
}

void set_leds(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_leds) return;
    for (int i = 0; i < kXiaoLedCount; i++) led_strip_set_pixel(s_leds, i, r, g, b);
    led_strip_refresh(s_leds);
}

void set_amp(bool) {}

bool key_pressed(Key key) {
    if (key != Key::Key1) return false;
#ifdef BOOKBOOK_USE_BOOT_BUTTON
    if (gpio_get_level(kXiaoButton) == 0) return true;  // active-low: switched to GND
#endif
#ifdef CONFIG_BOOKBOOK_TOUCH_ENABLED
    return touch::present();
#else
    return false;
#endif
}

#else  // Waveshare ESP32-S3-AUDIO-Board

static i2c_master_bus_handle_t s_i2c;
static esp_io_expander_handle_t s_expander;
static led_strip_handle_t s_leds;

esp_err_t init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = static_cast<gpio_num_t>(kI2cSda);
    bus_cfg.scl_io_num = static_cast<gpio_num_t>(kI2cScl);
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c), TAG, "i2c bus");

    esp_err_t exp_err = esp_io_expander_new_i2c_tca95xx_16bit(
        s_i2c, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &s_expander);
    if (exp_err == ESP_OK) {
        const uint32_t inputs = IO_EXPANDER_PIN_NUM_9 | IO_EXPANDER_PIN_NUM_10 | IO_EXPANDER_PIN_NUM_11;
        esp_io_expander_set_dir(s_expander, inputs, IO_EXPANDER_INPUT);
        esp_io_expander_set_dir(s_expander, IO_EXPANDER_PIN_NUM_8, IO_EXPANDER_OUTPUT);
        esp_io_expander_set_level(s_expander, IO_EXPANDER_PIN_NUM_8, 0);
    } else {
        s_expander = nullptr;
        ESP_LOGW(TAG, "TCA9555 not found (%s): no buttons/amp control", esp_err_to_name(exp_err));
    }

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = kLedGpio;
    strip_cfg.max_leds = kLedCount;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_leds), TAG, "leds");
    led_strip_clear(s_leds);
    return exp_err;  // LEDs are up; report a missing expander so callers can log it
}

void set_leds(uint8_t r, uint8_t g, uint8_t b) {
    if (!s_leds) return;
    for (int i = 0; i < kLedCount; i++) led_strip_set_pixel(s_leds, i, r, g, b);
    led_strip_refresh(s_leds);
}

void set_amp(bool on) {
    if (!s_expander) return;
    esp_io_expander_set_level(s_expander, IO_EXPANDER_PIN_NUM_8, on ? 1 : 0);
}

bool key_pressed(Key key) {
    if (!s_expander) return false;
    const uint32_t mask = 1u << static_cast<int>(key);
    uint32_t level = 0;
    if (esp_io_expander_get_level(s_expander, mask, &level) != ESP_OK) return false;
    return (level & mask) == 0;  // active low
}

#endif

}  // namespace board
