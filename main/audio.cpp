#include "audio.h"

#include "sdkconfig.h"

// Slider percent -> amplitude is squared so the slider feels even in loudness (80% = -3.9 dB, 50% = -12 dB).
static volatile int s_volume = CONFIG_BOOKBOOK_SPEAKER_VOLUME;
namespace audio {
void set_volume(int percent) { s_volume = percent < 1 ? 1 : (percent > 100 ? 100 : percent); }
}

#if CONFIG_BOOKBOOK_BOARD_XIAO_ESP32S3

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "audio";

namespace audio {

static i2s_chan_handle_t s_tx;
constexpr gpio_num_t kSdMode = static_cast<gpio_num_t>(CONFIG_BOOKBOOK_I2S_SD_MODE);

esp_err_t init() {
    gpio_config_t sd = {};
    sd.pin_bit_mask = 1ULL << kSdMode;
    sd.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&sd), TAG, "sd pin");
    gpio_set_level(kSdMode, 0);  // amp shut down

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;     // 8 x 480 frames = 240 ms of cushion against network jitter
    chan_cfg.dma_frame_num = 480;
    chan_cfg.auto_clear = true;    // underrun plays silence, not a repeating buzz
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx, nullptr), TAG, "new channel");

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRateHz);
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(CONFIG_BOOKBOOK_I2S_BCLK);
    std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(CONFIG_BOOKBOOK_I2S_LRC);
    std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(CONFIG_BOOKBOOK_I2S_DIN);
    std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    return i2s_channel_init_std_mode(s_tx, &std_cfg);
}

esp_err_t begin() {
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable");
    gpio_set_level(kSdMode, 1);  // >1.4V on the Adafruit breakout = left channel, matches mono slot
    return ESP_OK;
}

esp_err_t write(const uint8_t* pcm, size_t len) {
    constexpr int kChunk = 1024;  // bytes per scaled block
    int16_t scaled[kChunk / 2];
    while (len >= 2) {
        size_t n = len < kChunk ? len : kChunk;
        n &= ~static_cast<size_t>(1);
        const int16_t* in = reinterpret_cast<const int16_t*>(pcm);
        for (size_t i = 0; i < n / 2; i++) {
            scaled[i] = static_cast<int16_t>((static_cast<int32_t>(in[i]) * s_volume * s_volume) / 10000);
        }
        size_t written = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_write(s_tx, scaled, n, &written, portMAX_DELAY), TAG, "write");
        pcm += n;
        len -= n;
    }
    return ESP_OK;
}

void end() {
    // Let the DMA ring drain, then stop the clock so the last samples don't loop as a buzz.
    vTaskDelay(pdMS_TO_TICKS(150));
    gpio_set_level(kSdMode, 0);
    i2s_channel_disable(s_tx);
}

}  // namespace audio

#else  // Waveshare: codec-based output not implemented yet

namespace audio {
esp_err_t init() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t begin() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t write(const uint8_t*, size_t) { return ESP_ERR_NOT_SUPPORTED; }
void end() {}
}  // namespace audio

#endif
