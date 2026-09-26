#include "setupnet.h"

#include <algorithm>
#include <atomic>
#include <cctype>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "webconfig.h"
#include "wifi.h"

static const char* TAG = "setupnet";

namespace {

constexpr int64_t kIdleCloseMs = static_cast<int64_t>(setupnet::kIdleCloseMinutes) * 60 * 1000;

std::atomic<bool> s_on_request{false};  // the network is up because the member asked (and so closes when idle)
std::atomic<int64_t> s_opened_ms{0};    // when it was last opened or extended
TaskHandle_t s_watcher = nullptr;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

// "Librarian-A1B2" -> "Librarian, A, 1, B, 2", so the text-to-speech voice reads the four characters one at a time.
std::string spoken_name(const std::string& name) {
    std::string out = "Librarian";
    const size_t dash = name.find('-');
    if (dash == std::string::npos) return name;
    for (size_t i = dash + 1; i < name.size(); i++) {
        out += ", ";
        out += name[i] == '0' ? "zero" : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[i]))));
    }
    return out;
}

void watcher_task(void*) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!s_on_request) continue;
        const int64_t last = std::max<int64_t>(s_opened_ms, webconfig::last_admin_activity_ms());
        if (now_ms() - last < kIdleCloseMs) continue;
        ESP_LOGI(TAG, "no setup page activity for %d minutes: closing the setup network", setupnet::kIdleCloseMinutes);
        s_on_request = false;
        webconfig::stop_captive_dns();
        wifi::disable_ap();
    }
}

}  // namespace

namespace setupnet {

esp_err_t open_on_request(Opened& out) {
    if (wifi::ap_enabled() && !s_on_request) {
        // Already up by itself (Wi-Fi is down): leave it alone, it is not timed out.
        out.name = wifi::ap_ssid();
        out.spoken = spoken_name(out.name);
        out.automatic = true;
        return ESP_OK;
    }
    if (!s_on_request) {
        esp_err_t err = wifi::enable_ap();
        if (err != ESP_OK) return err;
        webconfig::start_captive_dns();
        ESP_LOGI(TAG, "setup network opened on request; closes %d minutes after the last setup page use", kIdleCloseMinutes);
    }
    s_opened_ms = now_ms();  // opening again extends it
    s_on_request = true;
    if (!s_watcher) xTaskCreate(watcher_task, "setupnet", 3072, nullptr, 3, &s_watcher);
    out.name = wifi::ap_ssid();
    out.spoken = spoken_name(out.name);
    out.automatic = false;
    return ESP_OK;
}

CloseResult close_now() {
    if (!wifi::ap_enabled()) return CloseResult::WasNotOpen;
    s_on_request = false;
    webconfig::stop_captive_dns();
    wifi::disable_ap();
    ESP_LOGI(TAG, "setup network turned off on request");
    return CloseResult::Closed;
}

}  // namespace setupnet
