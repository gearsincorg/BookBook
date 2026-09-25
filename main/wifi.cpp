#include "wifi.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "sdkconfig.h"

static const char* TAG = "wifi";
static constexpr const char* kApPassword = CONFIG_BOOKBOOK_SETUP_AP_PASSWORD;

static EventGroupHandle_t s_events;
constexpr int kConnected = BIT0;

static bool s_started;
static bool s_sta_enabled;
static bool s_ap_enabled;
static std::string s_sta_ssid, s_sta_pass, s_ap_ssid;
static esp_netif_t* s_sta_netif;

static void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_enabled) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, kConnected);
        if (s_sta_enabled) esp_wifi_connect();  // always retry; this device is mains powered
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, kConnected);
    }
}

// Applies the current STA/AP flags to the driver, starting it if needed.
static esp_err_t apply() {
    wifi_mode_t mode = s_ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) return err;

    if (s_sta_enabled) {
        wifi_config_t sta = {};
        strncpy(reinterpret_cast<char*>(sta.sta.ssid), s_sta_ssid.c_str(), sizeof(sta.sta.ssid) - 1);
        strncpy(reinterpret_cast<char*>(sta.sta.password), s_sta_pass.c_str(), sizeof(sta.sta.password) - 1);
        err = esp_wifi_set_config(WIFI_IF_STA, &sta);
        if (err != ESP_OK) return err;
    }
    if (s_ap_enabled) {
        wifi_config_t ap = {};
        strncpy(reinterpret_cast<char*>(ap.ap.ssid), s_ap_ssid.c_str(), sizeof(ap.ap.ssid) - 1);
        ap.ap.ssid_len = static_cast<uint8_t>(s_ap_ssid.size());
        strncpy(reinterpret_cast<char*>(ap.ap.password), kApPassword, sizeof(ap.ap.password) - 1);
        ap.ap.channel = 1;
        ap.ap.max_connection = 4;
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (err != ESP_OK) return err;
    }
    if (!s_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_started = true;
        esp_wifi_set_ps(WIFI_PS_NONE);
    } else if (s_sta_enabled) {
        esp_wifi_connect();
    }
    return ESP_OK;
}

namespace wifi {

esp_err_t init() {
    s_events = xEventGroupCreate();
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
    return ESP_OK;
}

esp_err_t start_sta(const char* ssid, const char* password, int timeout_ms) {
    s_sta_ssid = ssid;
    s_sta_pass = password;
    s_sta_enabled = true;
    esp_err_t err = apply();
    if (err != ESP_OK) return err;
    EventBits_t bits = xEventGroupWaitBits(s_events, kConnected, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & kConnected) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t enable_ap() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char name[24];
    snprintf(name, sizeof(name), "BookBook-%02X%02X", mac[4], mac[5]);
    s_ap_ssid = name;
    s_ap_enabled = true;
    ESP_LOGI(TAG, "setup AP \"%s\" up", name);
    return apply();
}

bool connected() { return s_events && (xEventGroupGetBits(s_events) & kConnected); }

std::string ip() {
    if (!connected() || !s_sta_netif) return "";
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK) return "";
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
    return buf;
}

std::string ap_ssid() { return s_ap_enabled ? s_ap_ssid : ""; }

esp_err_t scan(std::vector<ScanEntry>& out) {
    out.clear();
    wifi_scan_config_t cfg = {};
    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) return err;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;
    std::vector<wifi_ap_record_t> recs(n);
    esp_wifi_scan_get_ap_records(&n, recs.data());
    for (uint16_t i = 0; i < n; i++) {
        std::string ssid(reinterpret_cast<const char*>(recs[i].ssid));
        if (ssid.empty()) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const ScanEntry& e) { return e.ssid == ssid; });
        if (it == out.end()) {
            out.push_back({ssid, recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN});
        } else if (recs[i].rssi > it->rssi) {
            it->rssi = recs[i].rssi;
        }
    }
    std::sort(out.begin(), out.end(), [](const ScanEntry& a, const ScanEntry& b) { return a.rssi > b.rssi; });
    return ESP_OK;
}

esp_err_t sync_time(int timeout_ms) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) return err;
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err == ESP_OK) {
        time_t now = time(nullptr);
        ESP_LOGI(TAG, "time synced: %s", ctime(&now));
    }
    return err;
}

}  // namespace wifi
