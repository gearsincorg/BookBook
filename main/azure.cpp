#include "azure.h"

#include <cstdio>
#include <string>

#include "audio.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "azure";

namespace azure {

esp_err_t probe_token(const char* region, const char* key) {
    char url[128];
    snprintf(url, sizeof(url), "https://%s.api.cognitive.microsoft.com/sts/v1.0/issueToken", region);

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;
    cfg.keep_alive_enable = true;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "Ocp-Apim-Subscription-Key", key);
    esp_http_client_set_post_field(client, "", 0);

    esp_err_t result = ESP_OK;
    for (int attempt = 1; attempt <= 2; attempt++) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t err = esp_http_client_perform(client);
        int ms = static_cast<int>((esp_timer_get_time() - t0) / 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "request %d failed: %s (%d ms)", attempt, esp_err_to_name(err), ms);
            result = err;
            break;
        }
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "request %d (%s): HTTP %d, %d bytes, %d ms", attempt,
                 attempt == 1 ? "cold" : "warm/keep-alive", status,
                 static_cast<int>(esp_http_client_get_content_length(client)), ms);
        if (status != 200) result = ESP_FAIL;
    }
    esp_http_client_cleanup(client);
    return result;
}

static void append_escaped(std::string& out, const char* text) {
    for (const char* p = text; *p; p++) {
        switch (*p) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += *p;
        }
    }
}

esp_err_t speak(const char* region, const char* key, const char* text, bool (*cancel)()) {
    constexpr const char* kVoice = "en-AU-NatashaNeural";
    std::string ssml = "<speak version='1.0' xml:lang='en-AU'><voice name='";
    ssml += kVoice;
    ssml += "'>";
    append_escaped(ssml, text);
    ssml += "</voice></speak>";

    char url[128];
    snprintf(url, sizeof(url), "https://%s.tts.speech.microsoft.com/cognitiveservices/v1", region);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 15000;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "Ocp-Apim-Subscription-Key", key);
    esp_http_client_set_header(client, "Content-Type", "application/ssml+xml");
    esp_http_client_set_header(client, "X-Microsoft-OutputFormat", "raw-16khz-16bit-mono-pcm");
    esp_http_client_set_header(client, "User-Agent", "BookBook");

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, static_cast<int>(ssml.size()));
    if (err == ESP_OK &&
        esp_http_client_write(client, ssml.data(), static_cast<int>(ssml.size())) < 0) {
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tts open/write failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "tts HTTP %d", status);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    uint8_t buf[2048];
    size_t total = 0;
    bool started = false;
    int carry = -1;  // odd trailing byte from the previous read
    while (true) {
        int off = 0;
        if (carry >= 0) {
            buf[0] = static_cast<uint8_t>(carry);
            off = 1;
            carry = -1;
        }
        int n = esp_http_client_read(client, reinterpret_cast<char*>(buf) + off, sizeof(buf) - off);
        if (n <= 0) break;
        if (cancel && cancel()) {
            ESP_LOGI(TAG, "tts cancelled");
            break;
        }
        n += off;
        if (n & 1) {
            carry = buf[n - 1];
            n--;
        }
        if (!started) {
            ESP_LOGI(TAG, "tts first audio after %d ms", static_cast<int>((esp_timer_get_time() - t0) / 1000));
            if (audio::begin() != ESP_OK) {
                err = ESP_FAIL;
                break;
            }
            started = true;
        }
        if (audio::write(buf, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
        total += n;
    }
    if (started) audio::end();
    ESP_LOGI(TAG, "tts done: %u bytes (%u ms audio), %d ms total", static_cast<unsigned>(total),
             static_cast<unsigned>(total / 2 * 1000 / audio::kSampleRateHz),
             static_cast<int>((esp_timer_get_time() - t0) / 1000));
    esp_http_client_cleanup(client);
    return total > 0 ? err : ESP_FAIL;
}

}  // namespace azure
