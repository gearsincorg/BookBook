#include "mic.h"

#include <cmath>

#include "sdkconfig.h"

#if CONFIG_BOOKBOOK_BOARD_XIAO_ESP32S3

#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "mic";

namespace mic {

static i2s_chan_handle_t s_rx;

esp_err_t init() {
    // PDM receive is only supported on I2S0 (i2s_pdm.c rejects I2S1), so ask for it by number
    // rather than relying on allocation order.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 320;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, nullptr, &s_rx), TAG, "new channel");

    i2s_pdm_rx_config_t cfg = {};
    cfg.clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(kSampleRateHz);
    cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;  // PDM clock = 128 x rate = 2.048 MHz, inside a mic's 1-3.25 MHz
    cfg.slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
#ifdef CONFIG_BOOKBOOK_MIC_RIGHT_SLOT
    cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;  // mic SEL tied to 3V3
    const char* kSlotName = "right";
#else
    cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_LEFT;   // mic SEL tied to GND
    const char* kSlotName = "left";
#endif
    cfg.gpio_cfg.clk = static_cast<gpio_num_t>(CONFIG_BOOKBOOK_MIC_CLK);
    cfg.gpio_cfg.din = static_cast<gpio_num_t>(CONFIG_BOOKBOOK_MIC_DAT);
    ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_rx, &cfg), TAG, "init pdm rx");
    ESP_LOGI(TAG, "PDM mic: CLK=GPIO%d DAT=GPIO%d %s slot, gain x%d", CONFIG_BOOKBOOK_MIC_CLK,
             CONFIG_BOOKBOOK_MIC_DAT, kSlotName, CONFIG_BOOKBOOK_MIC_GAIN);
    return ESP_OK;
}

static bool s_running;
static size_t s_skip;  // samples still to drop: start-up transient and mic settling

esp_err_t start() {
    if (s_running) return ESP_OK;
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable");
    s_skip = kSampleRateHz * 150 / 1000;
    s_running = true;
    return ESP_OK;
}

esp_err_t read(std::vector<int16_t>& out, int timeout_ms) {
    if (!s_running) return ESP_ERR_INVALID_STATE;
    int16_t buf[320];  // 20 ms
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx, buf, sizeof(buf), &bytes, pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_ERR_TIMEOUT) return ESP_OK;  // nothing ready yet
    if (err != ESP_OK) return err;
    size_t n = bytes / sizeof(int16_t);
    size_t off = std::min(s_skip, n);
    s_skip -= off;
    out.insert(out.end(), buf + off, buf + n);
    return ESP_OK;
}

void stop() {
    if (!s_running) return;
    i2s_channel_disable(s_rx);
    s_running = false;
}

// Remove DC offset (PDM mics have one), then apply gain with clipping.
void process(std::vector<int16_t>& pcm, Stats* stats) {
    if (stats) *stats = Stats();
    if (pcm.empty()) return;
    double mean = 0;
    for (int16_t s : pcm) mean += s;
    mean /= pcm.size();
    double sq = 0;
    int peak = 0;
    for (auto& s : pcm) {
        int v = static_cast<int>((s - mean) * CONFIG_BOOKBOOK_MIC_GAIN);
        v = std::max(-32768, std::min(32767, v));
        s = static_cast<int16_t>(v);
        sq += static_cast<double>(v) * v;
        peak = std::max(peak, std::abs(v));
    }
    if (stats) {
        stats->rms = static_cast<int>(std::sqrt(sq / pcm.size()));
        stats->peak = peak;
    }
}

esp_err_t record(std::vector<int16_t>& out, int ms, Stats* stats) {
    out.clear();
    ESP_RETURN_ON_ERROR(start(), TAG, "start");
    const size_t want = static_cast<size_t>(kSampleRateHz) * ms / 1000;
    esp_err_t err = ESP_OK;
    while (out.size() < want && err == ESP_OK) err = read(out, 1000);
    stop();
    if (err != ESP_OK) {
        out.clear();
        return err;
    }
    out.resize(want);
    process(out, stats);
    return ESP_OK;
}

}  // namespace mic

#else  // Waveshare: ES7210 input not implemented yet

namespace mic {
esp_err_t init() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t start() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t read(std::vector<int16_t>&, int) { return ESP_ERR_NOT_SUPPORTED; }
void stop() {}
void process(std::vector<int16_t>&, Stats*) {}
esp_err_t record(std::vector<int16_t>&, int, Stats*) { return ESP_ERR_NOT_SUPPORTED; }
}  // namespace mic

#endif
