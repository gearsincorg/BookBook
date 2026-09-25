#include "thinking.h"

#include <cmath>
#include <vector>

#include "audio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char* TAG = "thinking";

namespace {

constexpr int kRate = audio::kSampleRateHz;
constexpr int kBlockMs = 20;                      // stop latency
constexpr int kBlockSamples = kRate * kBlockMs / 1000;
constexpr int kGapMs = 1400;                      // silence after each chime (as Bookworm)
constexpr int kCycleMs = 10000;                   // pattern rotates every ~10 s
constexpr float kAmplitude = 0.25f * 32767.0f;    // soft: a background cue, not an alert

struct Note {
    float hz;
    int ms;
};
// Same shapes as Bookworm: soft ping, gentle rising pair, gentle falling pair.
const Note kPatterns[3][2] = {
    {{440.0f, 180}, {0.0f, 0}},
    {{523.0f, 140}, {659.0f, 140}},
    {{659.0f, 140}, {523.0f, 140}},
};

volatile bool s_stop;
TaskHandle_t s_task;
SemaphoreHandle_t s_done;

bool stopping() { return s_stop; }

void write_block(const int16_t* pcm) {
    audio::write(reinterpret_cast<const uint8_t*>(pcm), kBlockSamples * sizeof(int16_t));
}

void silence(int ms) {
    static const int16_t zeros[kBlockSamples] = {};
    for (int t = 0; t < ms && !stopping(); t += kBlockMs) write_block(zeros);
}

// Sine with ~50 ms (or a quarter of the note) fades in and out so there are no clicks.
void tone(float hz, int ms) {
    int total = kRate * ms / 1000;
    int fade = std::max(1, std::min(total / 4, kRate / 20));
    std::vector<int16_t> pcm(((total + kBlockSamples - 1) / kBlockSamples) * kBlockSamples, 0);
    for (int i = 0; i < total; i++) {
        float env = 1.0f;
        if (i < fade) env = static_cast<float>(i) / fade;
        else if (i > total - fade) env = static_cast<float>(total - i) / fade;
        pcm[i] = static_cast<int16_t>(kAmplitude * env * sinf(2.0f * 3.14159265f * hz * i / kRate));
    }
    for (size_t off = 0; off < pcm.size() && !stopping(); off += kBlockSamples) write_block(&pcm[off]);
}

void task(void*) {
    if (audio::begin() == ESP_OK) {
        tone(880.0f, 90);  // confirms the press registered, before any waiting
        silence(500);
        int index = 0;
        while (!stopping()) {
            const Note* notes = kPatterns[index % 3];
            int64_t cycle_start = esp_timer_get_time();
            while (!stopping() && (esp_timer_get_time() - cycle_start) / 1000 < kCycleMs) {
                for (int n = 0; n < 2 && notes[n].ms > 0 && !stopping(); n++) tone(notes[n].hz, notes[n].ms);
                silence(kGapMs);
            }
            index++;
        }
        audio::end();
    } else {
        ESP_LOGW(TAG, "speaker unavailable; no waiting sounds");
        while (!stopping()) vTaskDelay(pdMS_TO_TICKS(20));
    }
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

}  // namespace

namespace thinking {

void start() {
    if (s_task) return;
    s_stop = false;
    if (!s_done) s_done = xSemaphoreCreateBinary();
    xSemaphoreTake(s_done, 0);
    xTaskCreate(task, "thinking", 6144, nullptr, 4, &s_task);
}

void stop() {
    if (!s_task) return;
    s_stop = true;
    xSemaphoreTake(s_done, pdMS_TO_TICKS(3000));
    s_task = nullptr;
}

}  // namespace thinking
