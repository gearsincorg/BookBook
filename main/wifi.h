#pragma once
#include "esp_err.h"

namespace wifi {
// Starts station mode with power save disabled (lowest latency) and blocks up to timeout_ms for an IP.
esp_err_t connect(const char* ssid, const char* password, int timeout_ms);
// Sets the system clock over SNTP (needed for TLS certificate validity checks).
esp_err_t sync_time(int timeout_ms);
}
