// Phase 0 bring-up: LEDs, Key1 via TCA9555, Wi-Fi.
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "wifi.h"

static const char* TAG = "bookbook";

extern "C" void app_main() {
    ESP_ERROR_CHECK(board::init());

    // LED colour-order check: expect red, green, blue in turn.
    board::set_leds(40, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    board::set_leds(0, 40, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    board::set_leds(0, 0, 40);
    vTaskDelay(pdMS_TO_TICKS(400));
    board::set_leds(0, 0, 0);

    if (CONFIG_BOOKBOOK_WIFI_SSID[0] != '\0') {
        board::set_leds(20, 10, 0);  // amber: connecting
        esp_err_t err = wifi::connect(CONFIG_BOOKBOOK_WIFI_SSID, CONFIG_BOOKBOOK_WIFI_PASSWORD, 20000);
        ESP_LOGI(TAG, "wifi: %s", esp_err_to_name(err));
        if (err == ESP_OK) {
            board::set_leds(0, 0, 20);  // blue: connected
        } else {
            board::set_leds(40, 0, 0);  // red: failed
        }
    } else {
        ESP_LOGW(TAG, "no Wi-Fi SSID configured (secrets/sdkconfig.secrets)");
    }

    bool was_down = false;
    while (true) {
        bool down = board::key_pressed(board::Key::Key1);
        if (down != was_down) {
            ESP_LOGI(TAG, "Key1 %s", down ? "down" : "up");
            board::set_leds(0, down ? 60 : 0, 0);
            was_down = down;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
