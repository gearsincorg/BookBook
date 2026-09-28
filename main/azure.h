#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include "esp_err.h"

namespace azure {
// Requests a short-lived Speech token twice on one keep-alive connection and logs status and
// timings (never the token or key). Proves TLS, the CA bundle, clock, key and region all work.
esp_err_t probe_token(const char* region, const char* key);

// Synthesises `text` with an Azure neural voice and streams the raw 16 kHz mono PCM straight to
// audio::write() as it arrives. Logs time-to-first-audio and total time.
// Speech-to-text for one short utterance (Azure "short audio" REST, up to ~60 s): 16 kHz 16-bit mono
// PCM in, best transcript out. `status` receives Azure's RecognitionStatus (Success, NoMatch, ...).
esp_err_t transcribe(const char* region, const char* key, const int16_t* pcm, size_t samples,
                     std::string& text, std::string* status = nullptr, const char* language = "en-AU",
                     const std::atomic<bool>* cancel = nullptr);

// `cancel`, if given, is polled between audio chunks; returning true stops playback early.
// `on_start`, if given, is called once from the player task the moment the speaker is switched on, after the
// connection, synthesis and pre-buffering delay, so a light can show "speaking" only when sound is actually coming.
esp_err_t speak(const char* region, const char* key, const char* text, bool (*cancel)() = nullptr,
                void (*on_start)() = nullptr);
}
