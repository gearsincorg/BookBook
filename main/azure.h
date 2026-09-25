#pragma once
#include "esp_err.h"

namespace azure {
// Requests a short-lived Speech token twice on one keep-alive connection and logs status and
// timings (never the token or key). Proves TLS, the CA bundle, clock, key and region all work.
esp_err_t probe_token(const char* region, const char* key);

// Synthesises `text` with an Azure neural voice and streams the raw 16 kHz mono PCM straight to
// audio::write() as it arrives. Logs time-to-first-audio and total time.
esp_err_t speak(const char* region, const char* key, const char* text);
}
