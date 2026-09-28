#pragma once
#include <cstdint>
#include <vector>
#include "esp_err.h"

// Microphone input: 16 kHz, 16-bit, mono PCM (what Azure speech-to-text wants).
// Phil's VA board: PDM MEMS mic on I2S0 (PDM receive only works on I2S0 on the ESP32-S3, so the speaker output is
// pinned to I2S1).
namespace mic {

constexpr int kSampleRateHz = 16000;

struct Stats {
    int rms = 0;   // 0..32767, after gain
    int peak = 0;  // 0..32767, after gain
    float gain = 1.0f;  // multiplier applied (adaptive: chosen for this recording)
    // The recording as the microphone delivered it (DC removed, before gain). A shout into the mic that reads
    // LOWER here than normal speech, or has a high share of samples near its peak (a flat-topped wave),
    // means the microphone itself is overloaded.
    int raw_peak = 0;
    int raw_rms = 0;
    int process_ms = 0;  // how long process() itself took
    int raw_peak_ms = 0;      // where in the recording the raw peak is: a peak in the first few hundred ms is a start-up click
    float near_peak_pct = 0;  // share of samples within 10% of raw_peak: ~0.1-1% in clean speech, high when flat-topped
};

esp_err_t init();

// Streaming capture, for push-to-talk: start() when the button goes down, read() repeatedly while it
// is held (appends whatever has arrived, waiting up to timeout_ms), stop() on release, then
// process() the whole recording once (DC offset removal, 70 Hz high-pass, gain (fixed or adaptive)).
esp_err_t start();
esp_err_t read(std::vector<int16_t>& out, int timeout_ms);
void stop();
void process(std::vector<int16_t>& pcm, Stats* stats = nullptr);

// Records `ms` milliseconds (after discarding ~150 ms of start-up noise) and processes it as above. Blocks for
// the duration. Used by the setup page's microphone test.
esp_err_t record(std::vector<int16_t>& out, int ms, Stats* stats = nullptr);

}  // namespace mic
