#include "mic.h"

#include <cmath>

#include "sdkconfig.h"

#if CONFIG_BOOKBOOK_BOARD_XIAO_ESP32S3

#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
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

#ifdef CONFIG_BOOKBOOK_MIC_AUTO_GAIN
// The level of the loud parts of a recording: the 99.5th percentile of |sample - mean|, from a histogram (32
// counts per bin, no copy of the recording), so a click or bump does not set the gain for the whole thing.
static int loud_level(const std::vector<int16_t>& pcm, int mean) {
    constexpr int kBinShift = 5, kBins = 32768 >> kBinShift;
    uint32_t hist[kBins] = {};
    for (int16_t s : pcm) {
        const int a = std::abs(s - mean);
        hist[std::min(a >> kBinShift, kBins - 1)]++;
    }
    const uint32_t keep = static_cast<uint32_t>(pcm.size() * 995ull / 1000);
    uint32_t seen = 0;
    for (int b = 0; b < kBins; b++) {
        seen += hist[b];
        if (seen >= keep) return (b + 1) << kBinShift;
    }
    return 32768;
}
#endif

// The recording's mean, as an integer (a fraction of a sample less makes no difference).
static int mean_of(const std::vector<int16_t>& pcm) {
    int64_t sum = 0;
    for (int16_t s : pcm) sum += s;
    return static_cast<int>(sum / static_cast<int64_t>(pcm.size()));
}

// Remove DC offset (PDM mics have one), high-pass, then apply gain with clipping. Integer and single-precision
// maths only: the S3 has no double-precision hardware, and this runs over the whole recording after the button is
// released, so it is on the member's waiting time (the log line at the end reports the cost).
void process(std::vector<int16_t>& pcm, Stats* stats) {
    if (stats) *stats = Stats();
    if (pcm.empty()) return;
    const int64_t t0 = esp_timer_get_time();

    // The recording as the microphone delivered it (DC removed), for the diagnostics in Stats.
    int mean = mean_of(pcm);
    int raw_peak = 0;
    size_t raw_peak_at = 0;
    uint64_t raw_sq = 0;
    for (size_t i = 0; i < pcm.size(); i++) {
        const int a = std::abs(pcm[i] - mean);
        if (a > raw_peak) {
            raw_peak = a;
            raw_peak_at = i;
        }
        raw_sq += static_cast<uint32_t>(a * a);
    }
    size_t near_peak = 0;
    for (int16_t s : pcm) near_peak += std::abs(s - mean) * 10 >= raw_peak * 9;

    // High-pass at ~70 Hz. The microphone and the receive filter settle slowly after the clock starts, leaving a
    // decaying DC step at the start of a recording that one whole-recording mean cannot remove; it is heard as a
    // click on playback and looks like loud speech to the adaptive gain. Speech does not reach down to 70 Hz.
    {
        constexpr float kHighPassHz = 70.0f;
        const float a = 1.0f - 2.0f * static_cast<float>(M_PI) * kHighPassHz / kSampleRateHz;
        float y = 0.0f, x_prev = pcm[0];
        for (auto& s : pcm) {
            const float x = s;
            y = a * (y + x - x_prev);
            x_prev = x;
            s = static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, y)));
        }
        mean = mean_of(pcm);
    }

    float gain = CONFIG_BOOKBOOK_MIC_GAIN;
#ifdef CONFIG_BOOKBOOK_MIC_AUTO_GAIN
    gain = std::max(1.0f, std::min(static_cast<float>(CONFIG_BOOKBOOK_MIC_MAX_GAIN),
                                   static_cast<float>(CONFIG_BOOKBOOK_MIC_TARGET_PEAK) / loud_level(pcm, mean)));
#endif
    uint64_t sq = 0;
    int peak = 0;
    for (auto& s : pcm) {
        int v = static_cast<int>(std::lround((s - mean) * gain));
        v = std::max(-32768, std::min(32767, v));
        s = static_cast<int16_t>(v);
        sq += static_cast<uint32_t>(v * v);
        peak = std::max(peak, std::abs(v));
    }
    const int process_ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);
    if (stats) {
        stats->process_ms = process_ms;
        stats->rms = static_cast<int>(std::sqrt(static_cast<double>(sq) / pcm.size()));
        stats->peak = peak;
        stats->gain = gain;
        stats->raw_peak = raw_peak;
        stats->raw_rms = static_cast<int>(std::sqrt(static_cast<double>(raw_sq) / pcm.size()));
        stats->near_peak_pct = 100.0f * near_peak / pcm.size();
        stats->raw_peak_ms = static_cast<int>(raw_peak_at * 1000 / kSampleRateHz);
    }
    ESP_LOGI(TAG, "processed %u samples (%u ms of audio) in %d ms", static_cast<unsigned>(pcm.size()),
             static_cast<unsigned>(pcm.size() * 1000 / kSampleRateHz), process_ms);
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
