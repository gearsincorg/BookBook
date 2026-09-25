#pragma once
#include <cstdint>
#include <vector>
#include "esp_err.h"

// Microphone input: 16 kHz, 16-bit, mono PCM (what Azure speech-to-text wants).
// XIAO stand-in: PDM MEMS mic on I2S0 (PDM receive only works on I2S0 on the ESP32-S3, so the
// speaker output is pinned to I2S1). Waveshare: ES7210 ADC (not implemented yet).
namespace mic {

constexpr int kSampleRateHz = 16000;

struct Stats {
    int rms = 0;   // 0..32767, after gain
    int peak = 0;  // 0..32767, after gain
};

esp_err_t init();

// Streaming capture, for push-to-talk: start() when the button goes down, read() repeatedly while it
// is held (appends whatever has arrived, waiting up to timeout_ms), stop() on release, then
// process() the whole recording once (DC offset removal + gain).
esp_err_t start();
esp_err_t read(std::vector<int16_t>& out, int timeout_ms);
void stop();
void process(std::vector<int16_t>& pcm, Stats* stats = nullptr);

// Records `ms` milliseconds (after discarding ~150 ms of start-up noise), removes DC offset and
// applies the configured gain. Blocks for the duration.
esp_err_t record(std::vector<int16_t>& out, int ms, Stats* stats = nullptr);

}  // namespace mic
