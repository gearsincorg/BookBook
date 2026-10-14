#include "wifi.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "dhcpserver/dhcpserver.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "wifi";
static constexpr const char* kApPassword = CONFIG_BOOKBOOK_SETUP_AP_PASSWORD;

static EventGroupHandle_t s_events;
constexpr int kConnected = BIT0;
constexpr int kAttemptFailed = BIT1;  // a join we started ourselves was refused or lost

constexpr int kQuickRetries = 5;       // reconnects to the same network before choosing again
constexpr int kJoinMs = 12000;         // longest wait for one network
constexpr int kRoamPauseMs = 10000;    // between rounds of choosing while offline
constexpr int kRoamBudgetMs = 40000;   // one round of scanning and joining

static bool s_started;
static bool s_sta_enabled;
static bool s_have_sta_config;  // a network has been configured, so the driver may connect by itself
static bool s_ap_enabled;
static std::string s_ap_ssid;
static esp_netif_t* s_sta_netif;

static std::vector<wifi::Network> s_nets;  // in priority order; the first s_preferred are joined whenever visible
static size_t s_preferred;

// While a scan or a choice of network is under way the disconnect handler must leave the radio alone: a connect
// attempt in progress makes the driver refuse a scan, and a scan makes the access point briefly unreachable.
static std::atomic<bool> s_paused{false};
static std::atomic<int> s_fails{0};
static SemaphoreHandle_t s_radio;  // one scan / join sequence at a time
static TaskHandle_t s_roam;

static std::atomic<bool> s_scan_running{false};
static SemaphoreHandle_t s_cache_lock;
static std::vector<wifi::ScanEntry> s_scan_cache;
static bool s_scan_ok;

static void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_sta_enabled && s_have_sta_config && !s_paused) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
        ESP_LOGW(TAG, "disconnected from \"%.32s\", reason %d", reinterpret_cast<const char*>(ev->ssid), ev->reason);
        xEventGroupClearBits(s_events, kConnected);
        if (s_paused) {
            // Our own join attempt failed (a deliberate disconnect is not a failure).
            if (ev->reason != WIFI_REASON_ASSOC_LEAVE) xEventGroupSetBits(s_events, kAttemptFailed);
            return;
        }
        if (!s_sta_enabled) return;
        if (++s_fails < kQuickRetries) {
            esp_wifi_connect();  // a dropout: this device is mains powered, so try the same network again straight away
        } else if (s_roam) {
            xTaskNotifyGive(s_roam);  // the network is gone (or we have moved): look for another
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&ev->ip_info.ip));
        s_fails = 0;
        xEventGroupSetBits(s_events, kConnected);
    }
}

// Applies the current STA/AP flags to the driver, starting it if needed. The station's network is chosen
// separately (try_join).
static esp_err_t apply() {
    wifi_mode_t mode = s_ap_enabled ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) return err;

    if (s_ap_enabled) {
        wifi_config_t ap = {};
        strncpy(reinterpret_cast<char*>(ap.ap.ssid), s_ap_ssid.c_str(), sizeof(ap.ap.ssid) - 1);
        ap.ap.ssid_len = static_cast<uint8_t>(s_ap_ssid.size());
        strncpy(reinterpret_cast<char*>(ap.ap.password), kApPassword, sizeof(ap.ap.password) - 1);
        ap.ap.channel = 1;
        ap.ap.max_connection = 4;
        // The default beacon (every 102.4 ms) is picked up by the microphone as a click at that rate. About 500 ms
        // (the unit is 1.024 ms) gives five times fewer, and phones still find and join the network.
        ap.ap.beacon_interval = 488;
        ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (err != ESP_OK) return err;
    }
    if (!s_started) {
        err = esp_wifi_start();
        if (err != ESP_OK) return err;
        s_started = true;
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    return ESP_OK;
}

static int64_t now_ms() { return esp_timer_get_time() / 1000; }

// Holds the radio for a scan or a choice of network, and keeps the disconnect handler from reconnecting meanwhile.
struct RadioLock {
    bool ok;
    explicit RadioLock(int wait_ms) {
        ok = xSemaphoreTake(s_radio, pdMS_TO_TICKS(wait_ms)) == pdTRUE;
        if (ok) s_paused = true;
    }
    ~RadioLock() {
        if (!ok) return;
        s_paused = false;
        xSemaphoreGive(s_radio);
    }
    RadioLock(const RadioLock&) = delete;
    RadioLock& operator=(const RadioLock&) = delete;
};

// Scans with the radio lock held. A connect attempt in progress makes the driver refuse ("busy"), so it is cancelled
// first (unless we are online, where scanning is fine), and a refusal is retried.
static esp_err_t scan_locked(std::vector<wifi::ScanEntry>& out) {
    out.clear();
    if (!wifi::connected()) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        wifi_scan_config_t cfg = {};
        err = esp_wifi_scan_start(&cfg, true);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "scan refused (%s), retrying", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (err != ESP_OK) return err;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 30) n = 30;
    std::vector<wifi_ap_record_t> recs(n);
    esp_wifi_scan_get_ap_records(&n, recs.data());
    for (uint16_t i = 0; i < n; i++) {
        std::string ssid(reinterpret_cast<const char*>(recs[i].ssid));
        if (ssid.empty()) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const wifi::ScanEntry& e) { return e.ssid == ssid; });
        if (it == out.end()) {
            out.push_back({ssid, recs[i].rssi, recs[i].authmode != WIFI_AUTH_OPEN});
        } else if (recs[i].rssi > it->rssi) {
            it->rssi = recs[i].rssi;
        }
    }
    std::sort(out.begin(), out.end(), [](const wifi::ScanEntry& a, const wifi::ScanEntry& b) { return a.rssi > b.rssi; });
    return ESP_OK;
}

// Tries one network (radio lock held). Gives up early when the access point refuses us (wrong password, not there).
static bool try_join(const wifi::Network& net, int timeout_ms) {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));  // let the deliberate disconnect's event pass before we start watching
    xEventGroupClearBits(s_events, kConnected | kAttemptFailed);
    wifi_config_t sta = {};
    strncpy(reinterpret_cast<char*>(sta.sta.ssid), net.ssid.c_str(), sizeof(sta.sta.ssid) - 1);
    strncpy(reinterpret_cast<char*>(sta.sta.password), net.password.c_str(), sizeof(sta.sta.password) - 1);
    if (esp_wifi_set_config(WIFI_IF_STA, &sta) != ESP_OK) return false;
    s_have_sta_config = true;
    ESP_LOGI(TAG, "joining \"%s\"", net.ssid.c_str());
    if (esp_wifi_connect() != ESP_OK) return false;
    EventBits_t bits = xEventGroupWaitBits(s_events, kConnected | kAttemptFailed, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return bits & kConnected;
}

// The order to try the networks in: a preferred (saved) network that is visible, then the visible ones strongest
// first, then any that were not seen (a hidden network does not appear in a scan). Without a scan, as listed.
static std::vector<size_t> try_order(const std::vector<wifi::ScanEntry>& seen, bool scanned) {
    std::vector<size_t> order;
    if (!scanned) {
        for (size_t i = 0; i < s_nets.size(); i++) order.push_back(i);
        return order;
    }
    auto rssi_of = [&](const std::string& ssid) {
        for (const auto& e : seen) if (e.ssid == ssid) return e.rssi;
        return INT_MIN;
    };
    std::vector<size_t> visible_pref, visible_other, unseen;
    for (size_t i = 0; i < s_nets.size(); i++) {
        if (rssi_of(s_nets[i].ssid) == INT_MIN) unseen.push_back(i);
        else if (i < s_preferred) visible_pref.push_back(i);
        else visible_other.push_back(i);
    }
    std::stable_sort(visible_other.begin(), visible_other.end(),
                     [&](size_t a, size_t b) { return rssi_of(s_nets[a].ssid) > rssi_of(s_nets[b].ssid); });
    order = visible_pref;
    order.insert(order.end(), visible_other.begin(), visible_other.end());
    order.insert(order.end(), unseen.begin(), unseen.end());
    return order;
}

// Scans, then joins the first network that works, within budget_ms. True if online afterwards.
static bool select_and_join(int budget_ms) {
    const int64_t deadline = now_ms() + budget_ms;
    RadioLock lock(budget_ms);
    if (!lock.ok) return false;
    if (wifi::connected()) return true;

    std::vector<wifi::ScanEntry> seen;
    bool scanned = scan_locked(seen) == ESP_OK && !seen.empty();
    ESP_LOGI(TAG, "scan %s, %d networks seen", scanned ? "ok" : "gave nothing", static_cast<int>(seen.size()));
    for (size_t i : try_order(seen, scanned)) {
        const int64_t left = deadline - now_ms();
        if (left < 1500) break;
        if (try_join(s_nets[i], static_cast<int>(std::min<int64_t>(left, kJoinMs)))) {
            ESP_LOGI(TAG, "joined \"%s\"", s_nets[i].ssid.c_str());
            return true;
        }
    }
    return false;
}

static bool ap_has_clients() { return wifi::ap_clients() > 0; }

// Chooses a network again whenever the one we had is lost for good (or was never reached). It leaves the radio
// alone while someone is using the setup network, because scanning would cut them off.
static void roam_task(void*) {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (!wifi::connected()) {
            vTaskDelay(pdMS_TO_TICKS(kRoamPauseMs));
            if (wifi::connected()) break;
            if (ap_has_clients()) continue;
            select_and_join(kRoamBudgetMs);
        }
    }
}

static void scan_task(void*) {
    std::vector<wifi::ScanEntry> found;
    esp_err_t err = wifi::scan(found);
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    s_scan_cache = std::move(found);
    s_scan_ok = err == ESP_OK;
    xSemaphoreGive(s_cache_lock);
    s_scan_running = false;
    vTaskDelete(nullptr);
}

namespace wifi {

esp_err_t init() {
    s_events = xEventGroupCreate();
    s_radio = xSemaphoreCreateMutex();
    s_cache_lock = xSemaphoreCreateMutex();
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_t* ap_netif = esp_netif_create_default_wifi_ap();
    // Tell phones on the setup network to use this device for DNS (webconfig's captive DNS answers every name with
    // its address). Without it they ask their own DNS server, so typed names such as librarian.local and the
    // automatic "sign in to network" page do not work and the IP address has to be typed.
    {
        esp_netif_ip_info_t ap_ip;
        if (esp_netif_get_ip_info(ap_netif, &ap_ip) == ESP_OK) {
            esp_netif_dns_info_t dns = {};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4 = ap_ip.ip;
            dhcps_offer_t offer_dns = OFFER_DNS;
            esp_netif_dhcps_stop(ap_netif);
            esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
            esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
            esp_netif_dhcps_start(ap_netif);
        }
    }
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) return err;
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
    return ESP_OK;
}

esp_err_t connect(const std::vector<Network>& networks, size_t preferred, int timeout_ms) {
    // Drop empty entries and exact duplicates (the same name and password listed twice), keeping the order.
    s_nets.clear();
    s_preferred = 0;
    for (size_t i = 0; i < networks.size(); i++) {
        const Network& n = networks[i];
        if (n.ssid.empty()) continue;
        bool dup = std::any_of(s_nets.begin(), s_nets.end(),
                               [&](const Network& m) { return m.ssid == n.ssid && m.password == n.password; });
        if (dup) continue;
        s_nets.push_back(n);
        if (i < preferred) s_preferred = s_nets.size();
    }
    if (s_nets.empty()) return ESP_ERR_INVALID_ARG;

    s_sta_enabled = true;
    esp_err_t err = apply();
    if (err != ESP_OK) return err;
    if (!s_roam) xTaskCreate(roam_task, "wifiroam", 4096, nullptr, 3, &s_roam);

    const bool online = select_and_join(timeout_ms);
    if (!online && s_roam) xTaskNotifyGive(s_roam);  // keep looking in the background
    return online ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t enable_ap() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char name[24];
    snprintf(name, sizeof(name), "Librarian-%02X%02X", mac[4], mac[5]);
    s_ap_ssid = name;
    s_ap_enabled = true;
    ESP_LOGI(TAG, "setup AP \"%s\" up", name);
    return apply();
}

esp_err_t disable_ap() {
    s_ap_enabled = false;
    ESP_LOGI(TAG, "setup AP down");
    return apply();
}

bool ap_enabled() { return s_ap_enabled; }

int ap_clients() {
    if (!s_ap_enabled) return 0;
    wifi_sta_list_t list = {};
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
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
    esp_err_t err;
    {
        RadioLock lock(30000);
        if (!lock.ok) return ESP_ERR_TIMEOUT;
        err = scan_locked(out);
    }
    // The scan cancelled any connect attempt in progress: start trying the network again.
    if (s_sta_enabled && s_have_sta_config && !connected()) {
        s_fails = 0;
        esp_wifi_connect();
    }
    return err;
}

void scan_begin() {
    if (s_scan_running.exchange(true)) return;
    if (xTaskCreate(scan_task, "wifiscan", 4096, nullptr, 3, nullptr) != pdPASS) s_scan_running = false;
}

ScanResult scan_result() {
    ScanResult r;
    r.running = s_scan_running;
    xSemaphoreTake(s_cache_lock, portMAX_DELAY);
    r.ok = s_scan_ok;
    r.networks = s_scan_cache;
    xSemaphoreGive(s_cache_lock);
    return r;
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
