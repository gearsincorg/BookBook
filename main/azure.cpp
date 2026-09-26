#include "azure.h"

#include <cstdio>
#include <string>

#include "audio.h"
#include "cJSON.h"

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static esp_err_t transcribe_once(const char* region, const char* key, const int16_t* pcm, size_t samples,
                                 std::string& text, std::string* status, const char* language) {
    text.clear();
    const uint32_t data_bytes = static_cast<uint32_t>(samples * 2);

    // Minimal 44-byte WAV header: PCM, mono, 16 kHz, 16-bit.
    uint8_t wav[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0,
                       0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    auto put32 = [&](int at, uint32_t v) {
        for (int i = 0; i < 4; i++) wav[at + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    put32(4, 36 + data_bytes);
    put32(24, audio::kSampleRateHz);
    put32(28, audio::kSampleRateHz * 2);
    put32(40, data_bytes);

    char url[192];
    snprintf(url, sizeof(url),
             "https://%s.stt.speech.microsoft.com/speech/recognition/conversation/cognitiveservices/v1?language=%s&format=simple",
             region, language);
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 10000;  // speech-to-text normally answers in about 1.5 s
    cfg.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;
    esp_http_client_set_header(client, "Ocp-Apim-Subscription-Key", key);
    esp_http_client_set_header(client, "Content-Type", "audio/wav; codecs=audio/pcm; samplerate=16000");
    esp_http_client_set_header(client, "Accept", "application/json");

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, static_cast<int>(sizeof(wav) + data_bytes));
    if (err == ESP_OK && esp_http_client_write(client, reinterpret_cast<const char*>(wav), sizeof(wav)) < 0) err = ESP_FAIL;
    const char* p = reinterpret_cast<const char*>(pcm);
    size_t left = data_bytes;
    while (err == ESP_OK && left > 0) {
        int n = static_cast<int>(left < 4096 ? left : 4096);
        if (esp_http_client_write(client, p, n) < 0) err = ESP_FAIL;
        p += n;
        left -= n;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stt open/upload failed: %s (free internal heap %u, largest block %u)", esp_err_to_name(err),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        esp_http_client_cleanup(client);
        return err;
    }
    esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);
    std::string body;
    char buf[512];
    int n;
    while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0 && body.size() < 8192) body.append(buf, n);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "stt: HTTP %d, %u audio bytes, %d ms (free internal heap %u, largest block %u)", http_status,
             static_cast<unsigned>(data_bytes), static_cast<int>((esp_timer_get_time() - t0) / 1000),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    if (http_status != 200) {
        ESP_LOGE(TAG, "stt HTTP %d: %.200s", http_status, body.c_str());
        return ESP_FAIL;
    }

    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    cJSON* st = cJSON_GetObjectItemCaseSensitive(root, "RecognitionStatus");
    cJSON* txt = cJSON_GetObjectItemCaseSensitive(root, "DisplayText");
    if (status && cJSON_IsString(st)) *status = st->valuestring;
    if (cJSON_IsString(txt) && txt->valuestring) text = txt->valuestring;
    cJSON_Delete(root);
    return ESP_OK;
}

// One automatic retry: a dropped connection or a busy moment should not cost the member a whole
// spoken request (weak Wi-Fi makes this more likely).
esp_err_t transcribe(const char* region, const char* key, const int16_t* pcm, size_t samples,
                     std::string& text, std::string* status, const char* language, const std::atomic<bool>* cancel) {
    esp_err_t err = transcribe_once(region, key, pcm, samples, text, status, language);
    if (err != ESP_OK && !(cancel && *cancel)) {  // no retry if the member has already given up
        ESP_LOGW(TAG, "stt failed (%s): retrying once", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(300));
        err = transcribe_once(region, key, pcm, samples, text, status, language);
    }
    return err;
}

}  // namespace azure
