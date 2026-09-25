#include "board.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_io_expander_tca95xx_16bit.h"
#include "led_strip.h"

static const char* TAG = "board";

namespace board {

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

    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca95xx_16bit(
                            s_i2c, ESP_IO_EXPANDER_I2C_TCA9555_ADDRESS_000, &s_expander),
                        TAG, "expander");
    const uint32_t inputs = IO_EXPANDER_PIN_NUM_9 | IO_EXPANDER_PIN_NUM_10 | IO_EXPANDER_PIN_NUM_11;
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, inputs, IO_EXPANDER_INPUT), TAG, "dir in");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, IO_EXPANDER_PIN_NUM_8, IO_EXPANDER_OUTPUT),
                        TAG, "dir out");
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, IO_EXPANDER_PIN_NUM_8, 0), TAG, "amp off");

    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = kLedGpio;
    strip_cfg.max_leds = kLedCount;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_leds), TAG, "leds");
    led_strip_clear(s_leds);
    return ESP_OK;
}

void set_leds(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < kLedCount; i++) led_strip_set_pixel(s_leds, i, r, g, b);
    led_strip_refresh(s_leds);
}

void set_amp(bool on) {
    esp_io_expander_set_level(s_expander, IO_EXPANDER_PIN_NUM_8, on ? 1 : 0);
}

bool key_pressed(Key key) {
    const uint32_t mask = 1u << static_cast<int>(key);
    uint32_t level = 0;
    if (esp_io_expander_get_level(s_expander, mask, &level) != ESP_OK) return false;
    return (level & mask) == 0;  // active low
}

}  // namespace board
