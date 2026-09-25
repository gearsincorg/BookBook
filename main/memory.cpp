#include "memory.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <functional>
#include <memory>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "va.h"

static const char* TAG = "memory";

namespace {

constexpr size_t kMaxBody = 512 * 1024;

struct Lock {
    static SemaphoreHandle_t handle() {
        static SemaphoreHandle_t h = xSemaphoreCreateRecursiveMutex();
        return h;
    }
    Lock() { xSemaphoreTakeRecursive(handle(), portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(handle()); }
};

cJSON* s_doc;        // the memory document (owned)
std::string s_etag;  // ETag of the version we last read or wrote (with quotes, as Azure sends it)
bool s_loaded;

struct Response {
    int status = 0;
    std::string body;
    std::string etag;
};

esp_err_t on_event(esp_http_client_event_t* e) {
    auto* r = static_cast<Response*>(e->user_data);
    if (!r) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_HEADER && strcasecmp(e->header_key, "ETag") == 0) {
        r->etag = e->header_value;
    } else if (e->event_id == HTTP_EVENT_ON_DATA && r->body.size() + e->data_len <= kMaxBody) {
        r->body.append(static_cast<const char*>(e->data), e->data_len);
    }
    return ESP_OK;
}

// One request to the blob's SAS URL. `if_match` / `if_none_match` are optional preconditions.
esp_err_t http(const Config& c, esp_http_client_method_t method, const std::string* body, const std::string& if_match,
               bool if_none_match_star, Response& out) {
    esp_http_client_config_t cfg = {};
    cfg.url = c.memory_url.c_str();
    cfg.method = method;
    cfg.event_handler = on_event;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    cfg.user_data = &out;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "x-ms-version", "2020-12-06");
    if (method == HTTP_METHOD_PUT) {
        esp_http_client_set_header(client, "x-ms-blob-type", "BlockBlob");
        esp_http_client_set_header(client, "Content-Type", "application/json");
        if (!if_match.empty()) esp_http_client_set_header(client, "If-Match", if_match.c_str());
        if (if_none_match_star) esp_http_client_set_header(client, "If-None-Match", "*");
        esp_http_client_set_post_field(client, body ? body->data() : "", body ? static_cast<int>(body->size()) : 0);
    }
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) out.status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

cJSON* new_doc() {
    cJSON* d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "ExplicitPreferences", cJSON_CreateArray());
    cJSON_AddItemToObject(d, "ConversationNotes", cJSON_CreateArray());
    cJSON_AddItemToObject(d, "LastSessionSummary", cJSON_CreateNull());
    cJSON_AddItemToObject(d, "ReadingHistory", cJSON_CreateArray());
    cJSON_AddItemToObject(d, "PreferredAuthors", cJSON_CreateArray());
    cJSON_AddItemToObject(d, "PreferredGenres", cJSON_CreateArray());
    return d;
}

// The array `name`, created if missing (Bookworm's files may predate a field).
cJSON* arr(const char* name) {
    if (!s_doc) s_doc = new_doc();
    cJSON* a = cJSON_GetObjectItemCaseSensitive(s_doc, name);
    if (!cJSON_IsArray(a)) {
        cJSON_DeleteItemFromObjectCaseSensitive(s_doc, name);
        a = cJSON_CreateArray();
        cJSON_AddItemToObject(s_doc, name, a);
    }
    return a;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return std::tolower(ch); });
    return s;
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    return lower(hay).find(lower(needle)) != std::string::npos;
}

std::string str(const cJSON* obj, const char* key) {
    const cJSON* it = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    return cJSON_IsString(it) && it->valuestring ? it->valuestring : "";
}

// "Silva, Daniel, 1960-" and "Daniel Silva" name the same person.
std::string author_key(const std::string& name) { return lower(va::natural_author(name)); }

std::string now_iso() {
    time_t t = time(nullptr);
    if (t < 1700000000) return "";  // clock not set yet
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    char buf[40];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm_utc);
    return buf;
}

std::string print(cJSON* j, bool take = true) {
    char* s = cJSON_PrintUnformatted(j);
    std::string out = s ? s : "";
    cJSON_free(s);
    if (take) cJSON_Delete(j);
    return out;
}

esp_err_t load_locked(const Config& c) {
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    Response r;
    esp_err_t err = http(c, HTTP_METHOD_GET, nullptr, "", false, r);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "load failed: %s", esp_err_to_name(err));
        return err;
    }
    cJSON* doc = nullptr;
    if (r.status == 200) {
        doc = cJSON_ParseWithLength(r.body.data(), r.body.size());
        if (!cJSON_IsObject(doc)) {
            cJSON_Delete(doc);
            ESP_LOGW(TAG, "load: memory.json is not a JSON object");
            return ESP_ERR_INVALID_RESPONSE;
        }
        s_etag = r.etag;
    } else if (r.status == 404) {
        doc = new_doc();  // first use: nothing saved yet
        s_etag.clear();
    } else {
        ESP_LOGW(TAG, "load: HTTP %d: %.200s", r.status, r.body.c_str());
        return ESP_FAIL;
    }
    cJSON_Delete(s_doc);
    s_doc = doc;
    s_loaded = true;
    return ESP_OK;
}

// Apply `mutate` and save. On an ETag conflict (Bookworm wrote in between) re-read and apply again once.
esp_err_t update(const Config& c, const std::function<void()>& mutate) {
    Lock lock;
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    if (!s_loaded) {
        esp_err_t err = load_locked(c);
        if (err != ESP_OK) return err;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        mutate();
        std::string body = print(s_doc, false);
        Response r;
        esp_err_t err = http(c, HTTP_METHOD_PUT, &body, s_etag, s_etag.empty(), r);
        if (err == ESP_OK && (r.status == 200 || r.status == 201)) {
            s_etag = r.etag;
            ESP_LOGI(TAG, "saved (%u bytes)", static_cast<unsigned>(body.size()));
            return ESP_OK;
        }
        bool conflict = err == ESP_OK && (r.status == 412 || r.status == 409);
        ESP_LOGW(TAG, "save %s: %s HTTP %d: %.200s", conflict ? "conflict" : "failed", esp_err_to_name(err), r.status,
                 r.body.c_str());
        // Drop our unsaved change and go back to what is really stored.
        if (load_locked(c) != ESP_OK || !conflict) return ESP_FAIL;
    }
    return ESP_FAIL;
}

}  // namespace

namespace memory {

bool configured(const Config& c) { return !c.memory_url.empty(); }

esp_err_t load(const Config& c) {
    Lock lock;
    return load_locked(c);
}

bool loaded() {
    Lock lock;
    return s_loaded;
}

Counts counts() {
    Lock lock;
    Counts n;
    if (!s_doc) return n;
    n.preferences = cJSON_GetArraySize(arr("ExplicitPreferences"));
    n.notes = cJSON_GetArraySize(arr("ConversationNotes"));
    n.authors = cJSON_GetArraySize(arr("PreferredAuthors"));
    n.genres = cJSON_GetArraySize(arr("PreferredGenres"));
    n.history = cJSON_GetArraySize(arr("ReadingHistory"));
    return n;
}

std::string prompt_snapshot() {
    Lock lock;
    if (!s_loaded || !s_doc) return "{}";
    cJSON* o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "explicitPreferences", cJSON_Duplicate(arr("ExplicitPreferences"), true));
    cJSON_AddItemToObject(o, "conversationNotes", cJSON_Duplicate(arr("ConversationNotes"), true));
    const cJSON* last = cJSON_GetObjectItemCaseSensitive(s_doc, "LastSessionSummary");
    if (cJSON_IsString(last)) cJSON_AddStringToObject(o, "lastSessionSummary", last->valuestring);
    cJSON* authors = cJSON_AddArrayToObject(o, "preferredAuthors");
    cJSON* a;
    cJSON_ArrayForEach(a, arr("PreferredAuthors")) {
        cJSON* x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "author", va::natural_author(str(a, "AuthorName")).c_str());
        const cJSON* fav = cJSON_GetObjectItemCaseSensitive(a, "IsFavorite");
        cJSON_AddBoolToObject(x, "isFavorite", cJSON_IsTrue(fav));
        cJSON_AddItemToArray(authors, x);
    }
    cJSON_AddItemToObject(o, "preferredGenres", cJSON_Duplicate(arr("PreferredGenres"), true));
    cJSON_AddNumberToObject(o, "readingHistoryEntryCount", cJSON_GetArraySize(arr("ReadingHistory")));
    return print(o);
}

std::string recall() {
    Lock lock;
    cJSON* o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "explicitPreferences", cJSON_Duplicate(arr("ExplicitPreferences"), true));
    cJSON_AddItemToObject(o, "conversationNotes", cJSON_Duplicate(arr("ConversationNotes"), true));
    const cJSON* last = s_doc ? cJSON_GetObjectItemCaseSensitive(s_doc, "LastSessionSummary") : nullptr;
    if (cJSON_IsString(last)) cJSON_AddStringToObject(o, "lastSessionSummary", last->valuestring);
    return print(o);
}

esp_err_t remember(const Config& c, const std::string& note) {
    return update(c, [&] { cJSON_AddItemToArray(arr("ExplicitPreferences"), cJSON_CreateString(note.c_str())); });
}

std::string search_history(const std::string& query) {
    Lock lock;
    cJSON* out = cJSON_CreateArray();
    const cJSON* e;
    int shown = 0;
    cJSON_ArrayForEach(e, arr("ReadingHistory")) {
        if (!contains_ci(str(e, "Title"), query) && !contains_ci(str(e, "Author"), query)) continue;
        if (++shown > 20) break;  // keep the result small; the model can narrow the query
        cJSON* x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "title", str(e, "Title").c_str());
        cJSON_AddStringToObject(x, "author", va::natural_author(str(e, "Author")).c_str());
        cJSON_AddStringToObject(x, "dateAdded", str(e, "DateAdded").c_str());
        const cJSON* removed = cJSON_GetObjectItemCaseSensitive(e, "DateRemoved");
        if (cJSON_IsString(removed)) cJSON_AddStringToObject(x, "dateRemoved", removed->valuestring);
        const cJSON* rating = cJSON_GetObjectItemCaseSensitive(e, "Rating");
        if (cJSON_IsNumber(rating)) cJSON_AddNumberToObject(x, "rating", rating->valueint);
        cJSON_AddItemToArray(out, x);
    }
    return print(out);
}

esp_err_t rate(const Config& c, const std::string& title, int rating, std::string* matched_title, int* matches) {
    Lock lock;
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    if (!s_loaded && load_locked(c) != ESP_OK) return ESP_FAIL;
    int count = 0;
    std::string found;
    const cJSON* e;
    cJSON_ArrayForEach(e, arr("ReadingHistory")) {
        if (contains_ci(str(e, "Title"), title)) {
            count++;
            found = str(e, "Title");
        }
    }
    if (matches) *matches = count;
    if (count == 0) return ESP_ERR_NOT_FOUND;
    if (count > 1) return ESP_ERR_INVALID_SIZE;
    if (matched_title) *matched_title = found;
    return update(c, [&] {
        cJSON* it;
        cJSON_ArrayForEach(it, arr("ReadingHistory")) {
            if (str(it, "Title") != found) continue;
            cJSON_DeleteItemFromObjectCaseSensitive(it, "Rating");
            cJSON_AddNumberToObject(it, "Rating", rating);
        }
    });
}

std::string preferred_authors_json() {
    Lock lock;
    cJSON* out = cJSON_CreateArray();
    const cJSON* a;
    cJSON_ArrayForEach(a, arr("PreferredAuthors")) {
        cJSON* x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "author", va::natural_author(str(a, "AuthorName")).c_str());
        cJSON_AddBoolToObject(x, "isFavorite", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "IsFavorite")));
        cJSON_AddItemToArray(out, x);
    }
    return print(out);
}

std::string preferred_genres_json() {
    Lock lock;
    return print(arr("PreferredGenres"), false);
}

esp_err_t add_author(const Config& c, const std::string& name, bool favorite) {
    std::string natural = va::natural_author(name);
    if (natural.empty()) natural = name;
    return update(c, [&] {
        cJSON* a;
        cJSON_ArrayForEach(a, arr("PreferredAuthors")) {
            if (author_key(str(a, "AuthorName")) == lower(natural)) {
                cJSON_DeleteItemFromObjectCaseSensitive(a, "IsFavorite");
                cJSON_AddBoolToObject(a, "IsFavorite", favorite);
                return;
            }
        }
        cJSON* x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "AuthorName", natural.c_str());
        cJSON_AddBoolToObject(x, "IsFavorite", favorite);
        std::string when = now_iso();
        if (!when.empty()) cJSON_AddStringToObject(x, "DateAdded", when.c_str());
        cJSON_AddItemToArray(arr("PreferredAuthors"), x);
    });
}

esp_err_t add_genre(const Config& c, const std::string& genre) {
    return update(c, [&] {
        cJSON* g;
        cJSON_ArrayForEach(g, arr("PreferredGenres")) {
            if (cJSON_IsString(g) && lower(g->valuestring) == lower(genre)) return;  // already there
        }
        cJSON_AddItemToArray(arr("PreferredGenres"), cJSON_CreateString(genre.c_str()));
    });
}

AddInfo add_info(const std::string& author) {
    Lock lock;
    AddInfo info;
    if (!author.empty()) {
        std::string want = author_key(author);
        const cJSON* a;
        cJSON_ArrayForEach(a, arr("PreferredAuthors")) {
            if (author_key(str(a, "AuthorName")) == want) info.author_known = true;
        }
    }
    info.genres_json = print(arr("PreferredGenres"), false);
    return info;
}

esp_err_t log_added(const Config& c, const std::string& title, const std::string& author, const std::string& bookshare_id) {
    return update(c, [&] {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "Title", title.c_str());
        if (author.empty()) cJSON_AddNullToObject(e, "Author");
        else cJSON_AddStringToObject(e, "Author", author.c_str());
        if (bookshare_id.empty()) cJSON_AddNullToObject(e, "BookshareId");
        else cJSON_AddStringToObject(e, "BookshareId", bookshare_id.c_str());
        std::string when = now_iso();
        if (!when.empty()) cJSON_AddStringToObject(e, "DateAdded", when.c_str());
        cJSON_AddNullToObject(e, "DateRemoved");
        cJSON_AddNullToObject(e, "Rating");
        cJSON_AddItemToArray(arr("ReadingHistory"), e);
    });
}

esp_err_t log_removed(const Config& c, const std::string& bookshare_id) {
    if (bookshare_id.empty()) return ESP_OK;
    return update(c, [&] {
        // The most recent entry for this title that is still open (Bookworm's rule).
        cJSON* target = nullptr;
        cJSON* e;
        cJSON_ArrayForEach(e, arr("ReadingHistory")) {
            const cJSON* removed = cJSON_GetObjectItemCaseSensitive(e, "DateRemoved");
            if (str(e, "BookshareId") == bookshare_id && !cJSON_IsString(removed)) target = e;
        }
        if (!target) return;
        std::string when = now_iso();
        cJSON_DeleteItemFromObjectCaseSensitive(target, "DateRemoved");
        if (when.empty()) cJSON_AddNullToObject(target, "DateRemoved");
        else cJSON_AddStringToObject(target, "DateRemoved", when.c_str());
    });
}

}  // namespace memory
