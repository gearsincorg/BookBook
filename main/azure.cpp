#include "azure.h"

#include <atomic>
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
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
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

// Speech playback: the download loop fills `buffer`, this task plays it. It waits for about 1.5 s of
// audio before starting, so ordinary network jitter never reaches the speaker.
constexpr size_t kTtsBufferBytes = 384 * 1024;  // 12 s at 16 kHz mono 16-bit
constexpr size_t kTtsPrebufferBytes = 48 * 1024;  // 1.5 s

// Frees a buffer made by xStreamBufferCreateWithCaps. Plain vStreamBufferDelete leaves its two allocations behind
// (384 KB leaked per reply), and IDF 5.5's vStreamBufferDeleteWithCaps deletes the buffer with a queue call and
// corrupts the heap (assert "block already marked as free"), so do it by hand.
static void delete_tts_buffer(StreamBufferHandle_t buffer) {
    uint8_t* storage = nullptr;
    StaticStreamBuffer_t* control = nullptr;
    if (xStreamBufferGetStaticBuffers(buffer, &storage, &control) != pdTRUE) return;
    vStreamBufferDelete(buffer);  // static buffers: this only detaches, it frees nothing
    heap_caps_free(control);
    heap_caps_free(storage);
}

struct TtsPlayback {
    StreamBufferHandle_t buffer = nullptr;
    SemaphoreHandle_t finished = nullptr;
    std::atomic<bool> done{false};    // the download has ended
    std::atomic<bool> stop{false};    // cancelled: drop what is left
    std::atomic<bool> failed{false};
    int underruns = 0;
};

static void tts_player_task(void* arg) {
    TtsPlayback* p = static_cast<TtsPlayback*>(arg);
    while (!p->done && !p->stop && xStreamBufferBytesAvailable(p->buffer) < kTtsPrebufferBytes)
        vTaskDelay(pdMS_TO_TICKS(20));
    if (!p->stop && xStreamBufferBytesAvailable(p->buffer) > 0) {
        if (audio::begin() != ESP_OK) {
            p->failed = true;
            p->stop = true;
        } else {
            uint8_t chunk[2048];
            while (!p->stop) {
                size_t n = xStreamBufferReceive(p->buffer, chunk, sizeof(chunk), pdMS_TO_TICKS(100));
                if (n > 0) {
                    if (audio::write(chunk, n) != ESP_OK) {
                        p->failed = true;
                        p->stop = true;
                    }
                } else if (p->done && xStreamBufferBytesAvailable(p->buffer) == 0) {
                    break;
                } else if (!p->done) {
                    p->underruns++;  // the download fell behind the speaker
                }
            }
            audio::end();
        }
    }
    xSemaphoreGive(p->finished);
    vTaskDelete(nullptr);
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

    // Download and playback are decoupled by a large buffer in PSRAM (12 s of audio). Playing straight from
    // the network left only the 240 ms I2S cushion, so any network stall in a long reply became a gap.
    TtsPlayback play;
    play.buffer = xStreamBufferCreateWithCaps(kTtsBufferBytes, 1, MALLOC_CAP_SPIRAM);
    if (!play.buffer) {
        ESP_LOGE(TAG, "tts: no memory for the audio buffer");
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    play.finished = xSemaphoreCreateBinary();
    if (xTaskCreate(tts_player_task, "tts_play", 6144, &play, 5, nullptr) != pdPASS) {
        vSemaphoreDelete(play.finished);
        delete_tts_buffer(play.buffer);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    uint8_t buf[2048];
    size_t total = 0;
    bool first = true;
    int carry = -1;  // odd trailing byte from the previous read
    while (!play.stop) {
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
            play.stop = true;
            break;
        }
        n += off;
        if (n & 1) {
            carry = buf[n - 1];
            n--;
        }
        if (first) {
            ESP_LOGI(TAG, "tts first audio after %d ms", static_cast<int>((esp_timer_get_time() - t0) / 1000));
            first = false;
        }
        size_t sent = 0;
        while (sent < static_cast<size_t>(n) && !play.stop) {  // blocks while the buffer is full
            sent += xStreamBufferSend(play.buffer, buf + sent, n - sent, pdMS_TO_TICKS(100));
            if (cancel && cancel()) play.stop = true;
        }
        total += sent;
    }
    int64_t downloaded_ms = (esp_timer_get_time() - t0) / 1000;
    play.done = true;  // no more audio is coming; the player drains the buffer and finishes
    // The download is usually far ahead of the speaker, so a touch has to be noticed here, while the buffer drains.
    while (xSemaphoreTake(play.finished, pdMS_TO_TICKS(20)) != pdTRUE) {
        if (cancel && cancel()) play.stop = true;  // the player stops at its next read and gives `finished`
    }
    ESP_LOGI(TAG, "tts done: %u bytes (%u ms audio), downloaded in %d ms, %d ms total, %d underruns",
             static_cast<unsigned>(total), static_cast<unsigned>(total / 2 * 1000 / audio::kSampleRateHz),
             static_cast<int>(downloaded_ms), static_cast<int>((esp_timer_get_time() - t0) / 1000), play.underruns);
    esp_err_t result = play.failed ? ESP_FAIL : err;
    vSemaphoreDelete(play.finished);
    delete_tts_buffer(play.buffer);
    esp_http_client_cleanup(client);
    return total > 0 ? result : ESP_FAIL;
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
