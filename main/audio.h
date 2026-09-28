#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

// Speaker output. 16 kHz, 16-bit, mono PCM (Azure's raw-16khz-16bit-mono-pcm format).
// Phil's VA board: MAX98357A I2S amp.
namespace audio {

constexpr int kSampleRateHz = 16000;

esp_err_t init();
void set_volume(int percent);                        // device scale 25-100 (lower is raised to 25), takes effect immediately

// The member-facing volume runs 0-100 (the setup page's slider, and what she says): 0 is the quietest useful level,
// which is 25 on the device scale, and 100 is the loudest. The device scale is what is stored and passed to set_volume.
int volume_from_level(int level);                    // 0-100 -> device scale
int level_from_volume(int volume);                   // device scale -> 0-100
esp_err_t begin();                                   // enable the amp and I2S clock
esp_err_t write(const uint8_t* pcm, size_t len);     // blocking; applies software volume
void end();                                          // flush and mute the amp

}  // namespace audio
