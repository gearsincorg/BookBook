#include "ota.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi.h"

static const char* TAG = "ota";

namespace {

constexpr const char* kImageBlob = "bookbook.bin";
constexpr const char* kManifestBlob = "bookbook.json";
constexpr size_t kMaxManifest = 4096;
constexpr const char* kNvsNamespace = "ota";
constexpr const char* kExpectKey = "expect";  // ELF sha256 of the image being installed

std::atomic<bool> s_install_requested{false};

// The memory URL points at .../memory.json?<sas>; the same container token reaches the firmware blobs.
std::string blob_url(const std::string& base, const char* blob) {
    size_t q = base.find('?');
    std::string path = base.substr(0, q);
    std::string query = q == std::string::npos ? "" : base.substr(q);
    size_t slash = path.rfind('/');
    return path.substr(0, slash + 1) + blob + query;
}

esp_err_t on_event(esp_http_client_event_t* e) {
    auto* body = static_cast<std::string*>(e->user_data);
    if (e->event_id == HTTP_EVENT_ON_DATA && body && body->size() + e->data_len <= kMaxManifest) {
        body->append(static_cast<const char*>(e->data), e->data_len);
    }
    return ESP_OK;
}

esp_err_t fetch_manifest(const std::string& url, std::string& body, int& status) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.event_handler = on_event;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 12000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    cfg.user_data = &body;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

std::string hex(const uint8_t* bytes, size_t n) {
    std::string s;
    char b[3];
    for (size_t i = 0; i < n; i++) {
        snprintf(b, sizeof b, "%02x", bytes[i]);
        s += b;
    }
    return s;
}

std::string str_field(cJSON* o, const char* name) {
    cJSON* f = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsString(f) && f->valuestring ? f->valuestring : "";
}

std::string built(const esp_app_desc_t& d) { return std::string(d.date) + " " + d.time; }

// The running image's full ELF hash. Not esp_app_get_elf_sha256(): it only keeps CONFIG_APP_RETRIEVE_LEN_ELF_SHA
// characters (9 by default, so 8 hex digits), which can never equal the 64 in the manifest.
std::string running_sha256() {
    const esp_app_desc_t* d = esp_app_get_description();
    return hex(d->app_elf_sha256, sizeof d->app_elf_sha256);
}

}  // namespace

namespace ota {

esp_err_t check(const Config& cfg, Info& out) {
    out = Info();
    if (cfg.memory_url.empty() || !wifi::connected()) return ESP_ERR_INVALID_STATE;

    const esp_app_desc_t* running = esp_app_get_description();
    out.running_version = running->version;
    out.running_built = built(*running);
    const std::string running_sha = running_sha256();

    std::string body;
    int status = 0;
    esp_err_t err = fetch_manifest(blob_url(cfg.memory_url, kManifestBlob), body, status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not read the update manifest: %s", esp_err_to_name(err));
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "manifest read: HTTP %d", status);
        return status == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    cJSON* m = cJSON_Parse(body.c_str());
    if (!m) return ESP_ERR_INVALID_RESPONSE;
    out.new_sha256 = str_field(m, "sha256");
    out.new_version = str_field(m, "version");
    out.new_built = str_field(m, "built");
    cJSON_Delete(m);
    if (out.new_sha256.size() != 64) return ESP_ERR_INVALID_RESPONSE;

    out.available = out.new_sha256 != running_sha;
    ESP_LOGI(TAG, "running %s (%s), published %s (%s): %s", out.running_version.c_str(), out.running_built.c_str(),
             out.new_version.c_str(), out.new_built.c_str(), out.available ? "update available" : "up to date");
    return ESP_OK;
}

void request_install() { s_install_requested = true; }
void cancel_install() { s_install_requested = false; }
bool install_requested() { return s_install_requested; }

esp_err_t install(const Config& cfg, std::string* why) {
    auto fail = [&](esp_err_t e, const char* reason) {
        ESP_LOGE(TAG, "install failed: %s (%s)", reason, esp_err_to_name(e));
        if (why) *why = reason;
        return e;
    };
    Info info;
    esp_err_t err = check(cfg, info);
    if (err != ESP_OK) return fail(err, "could not read the published version");
    if (!info.available) return fail(ESP_ERR_INVALID_STATE, "already up to date");

    const std::string url = blob_url(cfg.memory_url, kImageBlob);
    esp_http_client_config_t http = {};
    http.url = url.c_str();
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = 20000;
    http.buffer_size = 4096;
    http.buffer_size_tx = 2048;
    http.keep_alive_enable = true;
    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http;

    esp_https_ota_handle_t handle = nullptr;
    err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) return fail(err, "could not start the download");

    // The image must be the one the manifest describes: guards against a stale manifest or a half-published pair.
    esp_app_desc_t desc = {};
    err = esp_https_ota_get_img_desc(handle, &desc);
    if (err != ESP_OK || hex(desc.app_elf_sha256, sizeof desc.app_elf_sha256) != info.new_sha256) {
        esp_https_ota_abort(handle);
        return fail(ESP_ERR_INVALID_VERSION, "the downloaded image does not match the published version");
    }

    const int total = esp_https_ota_get_image_size(handle);
    int64_t last_log = 0;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int64_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_log > 2000) {
            last_log = now;
            ESP_LOGI(TAG, "downloaded %d of %d bytes", esp_https_ota_get_image_len_read(handle), total);
        }
    }
    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        return fail(err != ESP_OK ? err : ESP_FAIL, "the download was interrupted");
    }
    err = esp_https_ota_finish(handle);  // verifies the image and switches the boot partition
    if (err != ESP_OK) return fail(err, "the new image did not verify");

    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, kExpectKey, info.new_sha256.c_str());
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "update installed (%s), restarting", info.new_version.c_str());
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

BootReport take_boot_report() {
    nvs_handle_t nvs;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) != ESP_OK) return BootReport::None;
    char expected[65] = {};
    size_t len = sizeof expected;
    BootReport report = BootReport::None;
    if (nvs_get_str(nvs, kExpectKey, expected, &len) == ESP_OK) {
        report = running_sha256() == expected ? BootReport::Updated : BootReport::RolledBack;
        // Keep the note while the new image is still on probation (until mark_valid): if it crashes before then the
        // bootloader goes back to the old image, which finds the note and reports the failure.
        esp_ota_img_states_t state;
        bool probation = report == BootReport::Updated &&
                         esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
                         state == ESP_OTA_IMG_PENDING_VERIFY;
        if (!probation) {
            nvs_erase_key(nvs, kExpectKey);
            nvs_commit(nvs);
        }
        ESP_LOGI(TAG, "last update: %s", report == BootReport::Updated ? "installed" : "rolled back");
    }
    nvs_close(nvs);
    return report;
}

void mark_valid() {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "new image is running well: cancelling rollback");
        esp_ota_mark_app_valid_cancel_rollback();
        nvs_handle_t nvs;  // the update is confirmed: forget the note
        if (nvs_open(kNvsNamespace, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_erase_key(nvs, kExpectKey);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
    }
}

}  // namespace ota
