#include "config.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char* TAG = "config";
static const char* kNamespace = "bookbook";

static Config s_cfg;
static SemaphoreHandle_t s_lock;

struct StrField {
    const char* key;  // NVS key, max 15 chars
    std::string Config::*member;
};
static const StrField kStrFields[] = {
    {"wifi_ssid", &Config::wifi_ssid},   {"wifi_pass", &Config::wifi_password},
    {"va_user", &Config::va_user},       {"va_pass", &Config::va_password},
    {"az_key", &Config::azure_key},      {"az_region", &Config::azure_region},
    {"anth_key", &Config::anthropic_key}, {"admin_pw", &Config::admin_password},
};

namespace config {

void load() {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs init: %s", esp_err_to_name(err));

    Config cfg;  // Kconfig (developer) defaults first
    cfg.wifi_ssid = CONFIG_BOOKBOOK_WIFI_SSID;
    cfg.wifi_password = CONFIG_BOOKBOOK_WIFI_PASSWORD;
    cfg.azure_key = CONFIG_BOOKBOOK_AZURE_SPEECH_KEY;
    cfg.azure_region = CONFIG_BOOKBOOK_AZURE_SPEECH_REGION;
    cfg.volume = CONFIG_BOOKBOOK_SPEAKER_VOLUME;

    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) == ESP_OK) {
        for (const auto& f : kStrFields) {
            size_t len = 0;
            if (nvs_get_str(h, f.key, nullptr, &len) == ESP_OK && len > 1) {
                std::string v(len, '\0');
                if (nvs_get_str(h, f.key, v.data(), &len) == ESP_OK) {
                    v.resize(len - 1);
                    cfg.*(f.member) = v;
                }
            }
        }
        int32_t vol;
        if (nvs_get_i32(h, "volume", &vol) == ESP_OK) cfg.volume = vol;
        nvs_close(h);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = cfg;
    xSemaphoreGive(s_lock);
}

Config get() {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    Config copy = s_cfg;
    xSemaphoreGive(s_lock);
    return copy;
}

esp_err_t save(const Config& cfg) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    for (const auto& f : kStrFields) {
        err = nvs_set_str(h, f.key, (cfg.*(f.member)).c_str());
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_set_i32(h, "volume", cfg.volume);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return err;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = cfg;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

}  // namespace config
