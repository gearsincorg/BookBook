#include "webconfig.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "audio.h"
#include "azure.h"
#include "cJSON.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mdns.h"
#include "webpage.h"
#include "brain.h"
#include "memory.h"
#include "mic.h"
#include "thinking.h"
#include "va.h"
#include "wifi.h"

static const char* TAG = "web";
static bool s_trust_ap;
static bool s_dns_running;

// ---- helpers ---------------------------------------------------------------------------------

static bool peer_is_setup_ap(httpd_req_t* req) {
    int fd = httpd_req_to_sockfd(req);
    sockaddr_storage addr = {};
    socklen_t len = sizeof(addr);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
    uint32_t ip4 = 0;
    if (addr.ss_family == AF_INET) {
        ip4 = reinterpret_cast<sockaddr_in*>(&addr)->sin_addr.s_addr;
    } else if (addr.ss_family == AF_INET6) {
        auto* a6 = reinterpret_cast<sockaddr_in6*>(&addr);
        memcpy(&ip4, &a6->sin6_addr.un.u32_addr[3], 4);  // v4-mapped
    } else {
        return false;
    }
    return (ntohl(ip4) & 0xFFFFFF00u) == 0xC0A80400u;  // 192.168.4.0/24, the setup AP subnet
}

static bool authorized(httpd_req_t* req) {
    if (s_trust_ap && peer_is_setup_ap(req)) return true;

    char hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) == ESP_OK &&
        strncmp(hdr, "Basic ", 6) == 0) {
        unsigned char decoded[96];
        size_t olen = 0;
        if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &olen,
                                  reinterpret_cast<const unsigned char*>(hdr + 6), strlen(hdr + 6)) == 0) {
            decoded[olen] = 0;
            std::string want = "admin:" + config::get().admin_password;
            if (want == reinterpret_cast<char*>(decoded)) return true;
        }
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"BookBook setup\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Password required (user name: admin)", HTTPD_RESP_USE_STRLEN);
    return false;
}

static esp_err_t send_json(httpd_req_t* req, cJSON* root, const char* status = nullptr) {
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return httpd_resp_send_500(req);
    if (status) httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    cJSON_free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t* req, const char* status, const char* msg) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, o, status);
}

static std::string mac_string() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[20];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

// ---- handlers --------------------------------------------------------------------------------

static esp_err_t h_index(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, kIndexHtml, sizeof(kIndexHtml) - 1);
}

static esp_err_t h_get_config(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    Config c = config::get();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "wifi_ssid", c.wifi_ssid.c_str());
    cJSON_AddStringToObject(o, "va_user", c.va_user.c_str());
    cJSON_AddNumberToObject(o, "volume", c.volume);
    cJSON* has = cJSON_AddObjectToObject(o, "has");  // secrets are never sent back, only whether set
    cJSON_AddBoolToObject(has, "wifi_password", !c.wifi_password.empty());
    cJSON_AddBoolToObject(has, "va_password", !c.va_password.empty());
    cJSON_AddStringToObject(o, "mac", mac_string().c_str());
    cJSON_AddStringToObject(o, "ip", wifi::ip().c_str());
    cJSON_AddStringToObject(o, "ap", wifi::ap_ssid().c_str());
    return send_json(req, o);
}

static bool read_body(httpd_req_t* req, std::string& out) {
    constexpr size_t kMax = 4096;
    if (req->content_len == 0 || req->content_len > kMax) return false;
    out.resize(req->content_len);
    size_t got = 0;
    while (got < out.size()) {
        int n = httpd_req_recv(req, out.data() + got, out.size() - got);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

// Updates dst from JSON string field `name`. For secrets, an empty string keeps the old value.
static bool take_string(cJSON* root, const char* name, std::string& dst, size_t max_len, bool blank_keeps) {
    cJSON* it = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(it) || !it->valuestring) return true;  // absent: unchanged
    std::string v = it->valuestring;
    if (v.size() > max_len) return false;
    if (v.empty() && blank_keeps) return true;
    dst = v;
    return true;
}

static esp_err_t h_post_config(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    std::string body;
    if (!read_body(req, body)) return send_error(req, "400 Bad Request", "Bad request body");
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(body.c_str()), cJSON_Delete);
    if (!root) return send_error(req, "400 Bad Request", "Invalid JSON");

    Config before = config::get();
    Config c = before;
    bool ok = take_string(root.get(), "wifi_ssid", c.wifi_ssid, 32, false) &&
              take_string(root.get(), "wifi_password", c.wifi_password, 63, true) &&
              take_string(root.get(), "va_user", c.va_user, 80, false) &&
              take_string(root.get(), "va_password", c.va_password, 80, true);
    if (!ok) return send_error(req, "400 Bad Request", "A field is too long");

    cJSON* vol = cJSON_GetObjectItemCaseSensitive(root.get(), "volume");
    if (cJSON_IsNumber(vol)) {
        int v = static_cast<int>(vol->valuedouble);
        if (v < 1 || v > 100) return send_error(req, "400 Bad Request", "Volume must be 1 to 100");
        c.volume = v;
    }
    if (!c.wifi_password.empty() && c.wifi_password.size() < 8) {
        return send_error(req, "400 Bad Request", "Wi-Fi password must be at least 8 characters");
    }

    if (config::save(c) != ESP_OK) return send_error(req, "500 Internal Server Error", "Could not save");
    audio::set_volume(c.volume);

    bool restart = c.wifi_ssid != before.wifi_ssid || c.wifi_password != before.wifi_password;
    ESP_LOGI(TAG, "settings saved (restart needed: %s)", restart ? "yes" : "no");
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "restart_needed", restart);
    return send_json(req, o);
}

static esp_err_t h_scan(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    std::vector<wifi::ScanEntry> nets;
    if (wifi::scan(nets) != ESP_OK) return send_error(req, "503 Service Unavailable", "Scan busy, try again");
    cJSON* arr = cJSON_CreateArray();
    for (const auto& n : nets) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", n.ssid.c_str());
        cJSON_AddNumberToObject(o, "rssi", n.rssi);
        cJSON_AddBoolToObject(o, "secure", n.secure);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, arr);
}

static esp_err_t h_test_speak(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    if (!wifi::connected()) return send_error(req, "409 Conflict", "Not on the internet yet: save Wi-Fi and restart first");
    Config c = config::get();
    if (c.azure_key.empty()) return send_error(req, "409 Conflict", "No Azure key in this build");

    // Optional {"volume": N}: try that level for this test only, without saving it.
    int test_volume = c.volume;
    std::string body;
    if (read_body(req, body)) {
        std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(body.c_str()), cJSON_Delete);
        cJSON* vol = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "volume") : nullptr;
        if (cJSON_IsNumber(vol) && vol->valuedouble >= 1 && vol->valuedouble <= 100) {
            test_volume = static_cast<int>(vol->valuedouble);
        }
    }
    audio::set_volume(test_volume);
    ESP_LOGI(TAG, "speaker test at volume %d%% (saved: %d%%)", test_volume, c.volume);
    esp_err_t err = azure::speak(c.azure_region.c_str(), c.azure_key.c_str(),
                                 "Testing, one, two, three. Book Book is working.");
    audio::set_volume(c.volume);  // back to the saved level
    if (err != ESP_OK) return send_error(req, "502 Bad Gateway", "Azure speech request failed; check the key and region");
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static esp_err_t h_test_va(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    if (!wifi::connected()) return send_error(req, "409 Conflict", "Not on the internet yet: save Wi-Fi and restart first");
    Config c = config::get();
    std::string why;
    if (va::login(c.va_user, c.va_password, &why) != ESP_OK) {
        return send_error(req, "401 Unauthorized", ("Library login failed: " + why).c_str());
    }
    va::Shelf shelf;
    if (va::bookshelf(shelf) != ESP_OK) return send_error(req, "502 Bad Gateway", "Signed in, but could not read the bookshelf");
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "on_shelf", static_cast<double>(shelf.books.size()));
    cJSON_AddNumberToObject(o, "loan_count", shelf.loan_count);
    return send_json(req, o);
}

// Bring-up test for the microphone chain: beep, record 4 s, play it back (so you can judge level
// and clarity by ear), transcribe it with Azure, and say what was heard.
static esp_err_t h_test_mic(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    if (!wifi::connected()) return send_error(req, "409 Conflict", "Not on the internet yet: save Wi-Fi and restart first");
    Config c = config::get();
    if (c.azure_key.empty()) return send_error(req, "409 Conflict", "No Azure key in this build");

    constexpr int kRecordMs = 4000;
    thinking::beep();  // "speak now"
    std::vector<int16_t> pcm;
    mic::Stats stats;
    if (mic::record(pcm, kRecordMs, &stats) != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "Microphone not available");
    }
    ESP_LOGI(TAG, "mic test: %u samples, rms %d, peak %d", static_cast<unsigned>(pcm.size()), stats.rms, stats.peak);

    if (audio::begin() == ESP_OK) {  // play back exactly what was captured
        audio::write(reinterpret_cast<const uint8_t*>(pcm.data()), pcm.size() * sizeof(int16_t));
        audio::end();
    }

    std::string text, status;
    esp_err_t err = azure::transcribe(c.azure_region.c_str(), c.azure_key.c_str(), pcm.data(), pcm.size(), text, &status);
    std::string reply = err != ESP_OK ? "I could not reach the speech service."
                        : text.empty() ? "I did not hear anything I could understand."
                                       : "I heard: " + text;
    azure::speak(c.azure_region.c_str(), c.azure_key.c_str(), reply.c_str());

    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddStringToObject(o, "heard", text.c_str());
    cJSON_AddStringToObject(o, "status", status.c_str());
    cJSON_AddNumberToObject(o, "rms", stats.rms);
    cJSON_AddNumberToObject(o, "peak", stats.peak);
    return send_json(req, o);
}

// Types a request to the librarian instead of speaking it; returns the reply text (not spoken).
// Runs the same brain, so read-only library tools run against the real account.
static esp_err_t h_test_ask(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    if (!wifi::connected()) return send_error(req, "409 Conflict", "Not on the internet yet: save Wi-Fi and restart first");
    std::string body;
    if (!read_body(req, body)) return send_error(req, "400 Bad Request", "Bad request body");
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(body.c_str()), cJSON_Delete);
    cJSON* text = root ? cJSON_GetObjectItemCaseSensitive(root.get(), "text") : nullptr;
    if (!cJSON_IsString(text) || !text->valuestring[0]) return send_error(req, "400 Bad Request", "Missing text");
    std::string reply;
    esp_err_t err = brain::respond(config::get(), text->valuestring, reply);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddStringToObject(o, "reply", reply.c_str());
    return send_json(req, o);
}

// Reads the shared memory file and reports what is in it (counts only, never the contents).
static esp_err_t h_test_memory(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    if (!wifi::connected()) return send_error(req, "409 Conflict", "Not on the internet yet: save Wi-Fi and restart first");
    Config c = config::get();
    if (!memory::configured(c)) return send_error(req, "409 Conflict", "No memory storage URL in this build");
    if (memory::load(c) != ESP_OK) return send_error(req, "502 Bad Gateway", "Could not read the memory file; its token may have expired");
    memory::Counts n = memory::counts();
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "preferences", n.preferences);
    cJSON_AddNumberToObject(o, "authors", n.authors);
    cJSON_AddNumberToObject(o, "genres", n.genres);
    cJSON_AddNumberToObject(o, "history", n.history);
    cJSON_AddNumberToObject(o, "standby", n.standby);
    return send_json(req, o);
}

static esp_err_t h_reboot(httpd_req_t* req) {
    if (!authorized(req)) return ESP_OK;
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    send_json(req, o);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// Captive-portal probes and any unknown URL: send phones to the setup page.
static esp_err_t h_redirect(httpd_req_t* req) {
    if (!peer_is_setup_ap(req)) return httpd_resp_send_404(req);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, nullptr, 0);
}

// ---- captive DNS -----------------------------------------------------------------------------

static void dns_task(void*) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (sock < 0 || bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "captive DNS bind failed");
        if (sock >= 0) close(sock);
        s_dns_running = false;
        vTaskDelete(nullptr);
        return;
    }
    static const uint8_t kAnswer[] = {0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3C,
                                      0x00, 0x04, 192, 168, 4, 1};
    uint8_t buf[300];
    while (true) {
        sockaddr_in from = {};
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - sizeof(kAnswer), 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 12) continue;
        // Find the end of the single question (name, type, class).
        int i = 12;
        while (i < n && buf[i] != 0) i += buf[i] + 1;
        int qend = i + 1 + 4;
        if (qend > n) continue;
        uint16_t qtype = (buf[qend - 4] << 8) | buf[qend - 3];
        buf[2] = 0x81;  // response, recursion desired
        buf[3] = 0x80;  // recursion available
        buf[6] = 0;
        buf[7] = (qtype == 1) ? 1 : 0;  // answer only A queries; empty for AAAA etc.
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int out = qend;
        if (qtype == 1) {
            memcpy(buf + out, kAnswer, sizeof(kAnswer));
            out += sizeof(kAnswer);
        }
        sendto(sock, buf, out, 0, reinterpret_cast<sockaddr*>(&from), flen);
    }
}

namespace webconfig {

void set_trust_setup_ap(bool trust) { s_trust_ap = trust; }

void start_captive_dns() {
    if (s_dns_running) return;
    s_dns_running = true;
    xTaskCreate(dns_task, "dns", 4096, nullptr, 3, nullptr);
}

esp_err_t start(bool trust_setup_ap) {
    s_trust_ap = trust_setup_ap;

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("bookbook");
        mdns_instance_name_set("BookBook");
        mdns_service_add("BookBook setup", "_http", "_tcp", 80, nullptr, 0);
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 12288;  // the speaker test does a TLS request on the server task
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {"/", HTTP_GET, h_index, nullptr},
        {"/api/config", HTTP_GET, h_get_config, nullptr},
        {"/api/config", HTTP_POST, h_post_config, nullptr},
        {"/api/scan", HTTP_GET, h_scan, nullptr},
        {"/api/test/speak", HTTP_POST, h_test_speak, nullptr},
        {"/api/test/va", HTTP_POST, h_test_va, nullptr},
        {"/api/test/mic", HTTP_POST, h_test_mic, nullptr},
        {"/api/test/memory", HTTP_POST, h_test_memory, nullptr},
        {"/api/test/ask", HTTP_POST, h_test_ask, nullptr},
        {"/api/reboot", HTTP_POST, h_reboot, nullptr},
        {"/*", HTTP_GET, h_redirect, nullptr},  // last: captive-portal probes and unknown paths
    };
    for (const auto& r : routes) httpd_register_uri_handler(server, &r);
    ESP_LOGI(TAG, "setup page up (trust setup AP: %s)", trust_setup_ap ? "yes" : "no");
    return ESP_OK;
}

}  // namespace webconfig
