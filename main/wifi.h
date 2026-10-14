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

struct Network {
    std::string ssid;
    std::string password;
};

esp_err_t init();  // netif, event loop, Wi-Fi driver (once)

// Station mode with power save off (lowest latency). Scans, then joins the best network it can reach: the first
// `preferred` entries of `networks` are joined whenever they are visible, then the others, strongest first, then
// any that did not show up in the scan (hidden networks). A network that refuses us is skipped at once. Blocks up
// to timeout_ms for an IP; if that fails it keeps looking in the background (roaming), and again whenever the
// connection is lost for good. Keeps the setup AP if one is already enabled. Empty names are ignored.
esp_err_t connect(const std::vector<Network>& networks, size_t preferred, int timeout_ms);

// Adds the WPA2 setup access point "Librarian-XXXX" (password: CONFIG_BOOKBOOK_SETUP_AP_PASSWORD). Runs alongside the station.
esp_err_t enable_ap();

// Removes the setup access point again (the station stays connected).
esp_err_t disable_ap();
bool ap_enabled();
int ap_clients();  // devices (phones, PCs) currently joined to the setup network

bool connected();
std::string ip();       // station IPv4, empty if not connected
std::string ap_ssid();  // setup AP name, empty if not enabled

// Blocking scan. Pauses the station's reconnecting while it runs (a connect attempt in progress makes the driver
// refuse) and retries a refusal. Can take several seconds and takes the radio off the setup network's channel.
esp_err_t scan(std::vector<ScanEntry>& out);

// The same scan in the background, for the setup page: scan_begin() returns at once (and does nothing if one is
// already running); scan_result() says whether it is still running and, when not, what the last one found.
struct ScanResult {
    bool running = false;
    bool ok = false;
    std::vector<ScanEntry> networks;
};
void scan_begin();
ScanResult scan_result();

// Sets the system clock over SNTP (needed for TLS certificate validity checks).
esp_err_t sync_time(int timeout_ms);

}  // namespace wifi
