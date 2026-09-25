#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

// Speaker output. 16 kHz, 16-bit, mono PCM (Azure's raw-16khz-16bit-mono-pcm format).
// XIAO stand-in: MAX98357A I2S amp. Waveshare: ES8311 + NS4150B (not implemented yet).
namespace audio {

constexpr int kSampleRateHz = 16000;

esp_err_t init();
esp_err_t begin();                                   // enable the amp and I2S clock
esp_err_t write(const uint8_t* pcm, size_t len);     // blocking; applies software volume
void end();                                          // flush and mute the amp

}  // namespace audio
