// Phase 0-2 bring-up: board IO, Wi-Fi, web setup page, Azure speech test.
#include "audio.h"
#include "azure.h"
#include "board.h"
#include "config.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "webconfig.h"
#include "wifi.h"

static const char* TAG = "bookbook";

static void show_idle_state() {
    if (wifi::connected()) {
        board::set_leds(0, 0, 20);   // blue: online
    } else if (!wifi::ap_ssid().empty()) {
        board::set_leds(25, 0, 25);  // purple: setup network is up
    } else {
        board::set_leds(20, 10, 0);  // amber: connecting
    }
}

extern "C" void app_main() {
    config::load();
    Config cfg = config::get();

    if (board::init() != ESP_OK) {
        ESP_LOGW(TAG, "board init incomplete (not a Waveshare audio board?); continuing");
    }
    if (audio::init() != ESP_OK) ESP_LOGW(TAG, "audio output unavailable");
    audio::set_volume(cfg.volume);

    // LED colour-order check: expect red, green, blue in turn.
    board::set_leds(40, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    board::set_leds(0, 40, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    board::set_leds(0, 0, 40);
    vTaskDelay(pdMS_TO_TICKS(400));
    board::set_leds(20, 10, 0);

    // Note: Key1 cannot be checked at boot on the XIAO stand-in (BOOT is GPIO0, a strapping pin:
    // holding it at reset enters the ROM downloader). Setup mode is a 5 s hold while running.
    bool have_wifi = !cfg.wifi_ssid.empty();

    ESP_ERROR_CHECK(wifi::init());
    webconfig::start(/*trust_setup_ap=*/!have_wifi);

    bool online = false;
    if (have_wifi) {
        online = wifi::start_sta(cfg.wifi_ssid.c_str(), cfg.wifi_password.c_str(), 20000) == ESP_OK;
        ESP_LOGI(TAG, "wifi: %s", online ? "connected" : "failed, starting setup network");
    }
    if (!online) {
        wifi::enable_ap();
        webconfig::start_captive_dns();
    }

    if (online) {
        ESP_LOGI(TAG, "time sync: %s", esp_err_to_name(wifi::sync_time(15000)));
        ESP_LOGI(TAG, "setup page: http://%s/ or http://bookbook.local/", wifi::ip().c_str());
        if (!cfg.azure_key.empty()) {
            azure::speak(cfg.azure_region.c_str(), cfg.azure_key.c_str(),
                         "Hello. This is Book Book, speaking from an E S P 32.");
        }
    }
    show_idle_state();

    constexpr int kSetupHoldMs = 5000;
    bool was_down = false;
    bool setup_triggered = false;
    int64_t down_since_ms = 0;
    bool last_online = wifi::connected();
    while (true) {
        bool down = board::key_pressed(board::Key::Key1);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (down && !was_down) {
            ESP_LOGI(TAG, "Key1 down");
            board::set_leds(0, 60, 0);
            down_since_ms = now_ms;
            setup_triggered = false;
        } else if (down && !setup_triggered && now_ms - down_since_ms >= kSetupHoldMs) {
            // Long hold: bring up the setup network (station keeps running) and trust its clients.
            setup_triggered = true;
            ESP_LOGI(TAG, "Key1 held %d ms: starting setup network", kSetupHoldMs);
            wifi::enable_ap();
            webconfig::set_trust_setup_ap(true);
            webconfig::start_captive_dns();
            board::set_leds(25, 0, 25);  // purple
        } else if (!down && was_down) {
            ESP_LOGI(TAG, "Key1 up");
            Config c = config::get();
            if (!setup_triggered && wifi::connected() && !c.azure_key.empty()) {
                azure::speak(c.azure_region.c_str(), c.azure_key.c_str(),
                             "You pressed the button. I can hear you, well, feel you.");
            }
            show_idle_state();
        }
        was_down = down;
        if (!down && wifi::connected() != last_online) {
            last_online = wifi::connected();
            show_idle_state();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
