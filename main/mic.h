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

// Records `ms` milliseconds (after discarding ~150 ms of start-up noise), removes DC offset and
// applies the configured gain. Blocks for the duration.
esp_err_t record(std::vector<int16_t>& out, int ms, Stats* stats = nullptr);

}  // namespace mic
