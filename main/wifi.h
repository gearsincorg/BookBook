#pragma once
#include <string>
#include <vector>
#include "esp_err.h"

namespace wifi {

struct ScanEntry {
    std::string ssid;
    int rssi;
    bool secure;
};

esp_err_t init();  // netif, event loop, Wi-Fi driver (once)

// Station mode with power save off (lowest latency). Blocks up to timeout_ms for an IP; keeps
// retrying in the background afterwards. Keeps the setup AP if one is already enabled.
esp_err_t start_sta(const char* ssid, const char* password, int timeout_ms);

// Adds the WPA2 setup access point "BookBook-XXXX" (password: CONFIG_BOOKBOOK_SETUP_AP_PASSWORD). Runs alongside the station.
esp_err_t enable_ap();

bool connected();
std::string ip();       // station IPv4, empty if not connected
std::string ap_ssid();  // setup AP name, empty if not enabled
esp_err_t scan(std::vector<ScanEntry>& out);

// Sets the system clock over SNTP (needed for TLS certificate validity checks).
esp_err_t sync_time(int timeout_ms);

}  // namespace wifi
