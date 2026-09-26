#include "memory.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
int64_t s_last_sync_us;  // when either blob was last read or saved (see refresh())

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
esp_err_t http(const std::string& url, esp_http_client_method_t method, const std::string* body,
               const std::string& if_match, bool if_none_match_star, Response& out,
               const std::string& if_none_match = "") {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = method;
    cfg.event_handler = on_event;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 12000;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 2048;
    cfg.user_data = &out;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "x-ms-version", "2020-12-06");
    if (method == HTTP_METHOD_GET && !if_none_match.empty()) {
        esp_http_client_set_header(client, "If-None-Match", if_none_match.c_str());  // 304 if unchanged
    }
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

// GET with one retry on a transport error (a dropped connection or timeout on a weak link).
esp_err_t http_get(const std::string& url, Response& out, const std::string& if_none_match = "") {
    esp_err_t err = http(url, HTTP_METHOD_GET, nullptr, "", false, out, if_none_match);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed (%s): retrying once", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(300));
        out = Response();
        err = http(url, HTTP_METHOD_GET, nullptr, "", false, out, if_none_match);
    }
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
    esp_err_t err = http_get(c.memory_url, r);
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
    s_last_sync_us = esp_timer_get_time();
    return ESP_OK;
}

// Apply `mutate` and save `doc` to `url`, guarding with the blob's ETag. A save can fail on the way back
// (a dropped connection or a timeout while waiting for the answer) although the server already stored it, so
// after any failure the real stored copy is re-read and the change re-applied to it: if that changes nothing
// the write had in fact gone through and the save counts as done (this used to report a failure for a change
// that had worked); otherwise the change is written again. Every mutation is written to be repeatable.
esp_err_t save_doc(const std::string& url, cJSON*& doc, std::string& etag, const std::function<esp_err_t()>& reload,
                   const std::function<void()>& mutate, const char* what) {
    bool mutated = false;  // true when `doc` already holds the change on top of a fresh copy
    for (int attempt = 0; attempt < 3; attempt++) {
        if (!mutated) mutate();
        mutated = false;
        std::string body = print(doc, false);
        Response r;
        esp_err_t err = http(url, HTTP_METHOD_PUT, &body, etag, etag.empty(), r);
        if (err == ESP_OK && (r.status == 200 || r.status == 201)) {
            etag = r.etag;
            s_last_sync_us = esp_timer_get_time();
            ESP_LOGI(TAG, "%s saved (%u bytes)", what, static_cast<unsigned>(body.size()));
            return ESP_OK;
        }
        ESP_LOGW(TAG, "%s save attempt %d failed: %s HTTP %d: %.160s", what, attempt + 1, esp_err_to_name(err), r.status,
                 r.body.c_str());
        if (reload() != ESP_OK) return ESP_FAIL;  // cannot even read it back: report the failure
        const std::string stored = print(doc, false);
        mutate();
        if (print(doc, false) == stored) {
            ESP_LOGI(TAG, "%s: the change had already been saved", what);
            return ESP_OK;
        }
        mutated = true;  // not saved: doc now holds the change on top of what is stored; write that
    }
    reload();  // give up: drop the unsaved change and go back to what is really stored
    return ESP_FAIL;
}

esp_err_t update(const Config& c, const std::function<void()>& mutate) {
    Lock lock;
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    if (!s_loaded) {
        esp_err_t err = load_locked(c);
        if (err != ESP_OK) return err;
    }
    return save_doc(c.memory_url, s_doc, s_etag, [&] { return load_locked(c); }, mutate, "memory");
}

// ---- standby list: its own blob (see memory.h) --------------------------------------------------

cJSON* s_standby;  // {"Books":[...]} (owned)
std::string s_standby_etag;
bool s_standby_loaded;

// The memory URL points at .../memory.json?<sas>; the same container token reaches its other blobs.
std::string blob_url(const std::string& base, const char* blob) {
    size_t q = base.find('?');
    std::string path = base.substr(0, q);
    std::string query = q == std::string::npos ? "" : base.substr(q);
    size_t slash = path.rfind('/');
    return path.substr(0, slash + 1) + blob + query;
}

cJSON* standby_books() {
    if (!s_standby) s_standby = cJSON_CreateObject();
    cJSON* a = cJSON_GetObjectItemCaseSensitive(s_standby, "Books");
    if (!cJSON_IsArray(a)) {
        cJSON_DeleteItemFromObjectCaseSensitive(s_standby, "Books");
        a = cJSON_CreateArray();
        cJSON_AddItemToObject(s_standby, "Books", a);
    }
    return a;
}

esp_err_t standby_load_locked(const Config& c) {
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    Response r;
    esp_err_t err = http_get(blob_url(c.memory_url, "standby.json"), r);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "standby load failed: %s", esp_err_to_name(err));
        return err;
    }
    cJSON* doc = nullptr;
    if (r.status == 200) {
        doc = cJSON_ParseWithLength(r.body.data(), r.body.size());
        if (!cJSON_IsObject(doc)) {
            cJSON_Delete(doc);
            return ESP_ERR_INVALID_RESPONSE;
        }
        s_standby_etag = r.etag;
    } else if (r.status == 404) {
        doc = cJSON_CreateObject();  // nothing saved yet
        s_standby_etag.clear();
    } else {
        ESP_LOGW(TAG, "standby load: HTTP %d: %.200s", r.status, r.body.c_str());
        return ESP_FAIL;
    }
    cJSON_Delete(s_standby);
    s_standby = doc;
    s_standby_loaded = true;
    s_last_sync_us = esp_timer_get_time();
    return ESP_OK;
}

// Same guarded save as update(), for standby.json.
esp_err_t standby_update(const Config& c, const std::function<void()>& mutate) {
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    if (!s_standby_loaded) {
        esp_err_t err = standby_load_locked(c);
        if (err != ESP_OK) return err;
    }
    return save_doc(blob_url(c.memory_url, "standby.json"), s_standby, s_standby_etag,
                    [&] { return standby_load_locked(c); }, mutate, "standby");
}

cJSON* standby_find(const std::string& title, const std::string& bookshare_id) {
    cJSON* e;
    if (!bookshare_id.empty()) {
        cJSON_ArrayForEach(e, standby_books()) {
            if (str(e, "BookshareId") == bookshare_id) return e;
        }
    }
    if (!title.empty()) {
        cJSON_ArrayForEach(e, standby_books()) {
            if (lower(str(e, "Title")) == lower(title)) return e;
        }
    }
    return nullptr;
}

esp_err_t standby_ensure_loaded(const Config& c) {
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    return s_standby_loaded ? ESP_OK : standby_load_locked(c);
}

void standby_delete(const std::string& title) {
    cJSON* list = standby_books();
    int n = cJSON_GetArraySize(list);
    for (int i = 0; i < n; i++) {
        if (str(cJSON_GetArrayItem(list, i), "Title") == title) {
            cJSON_DeleteItemFromArray(list, i);
            return;
        }
    }
}

}  // namespace

namespace memory {

bool configured(const Config& c) { return !c.memory_url.empty(); }

// Replace `doc` with a fresh copy of `url` if it changed (conditional read). Returns ESP_OK if unchanged or updated.
static esp_err_t refresh_blob(const std::string& url, std::string& etag, cJSON*& doc, bool make_root_if_missing) {
    Response r;
    esp_err_t err = http_get(url, r, etag);
    if (err != ESP_OK) return err;
    if (r.status == 304) return ESP_OK;  // unchanged
    if (r.status == 200) {
        cJSON* fresh = cJSON_ParseWithLength(r.body.data(), r.body.size());
        if (!cJSON_IsObject(fresh)) {
            cJSON_Delete(fresh);
            return ESP_ERR_INVALID_RESPONSE;
        }
        cJSON_Delete(doc);
        doc = fresh;
        etag = r.etag;
        ESP_LOGI(TAG, "%s changed elsewhere: reloaded", url.substr(0, url.find('?')).c_str());
        return ESP_OK;
    }
    if (r.status == 404) return ESP_OK;  // deleted elsewhere: keep what we have
    (void)make_root_if_missing;
    return ESP_FAIL;
}

esp_err_t refresh(const Config& c) {
    Lock lock;
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    if (s_loaded && s_standby_loaded && esp_timer_get_time() - s_last_sync_us < 20LL * 1000 * 1000) return ESP_OK;
    esp_err_t result = ESP_OK;
    esp_err_t e1 = s_loaded ? refresh_blob(c.memory_url, s_etag, s_doc, false) : load_locked(c);
    esp_err_t e2 = s_standby_loaded ? refresh_blob(blob_url(c.memory_url, "standby.json"), s_standby_etag, s_standby, false)
                                    : standby_load_locked(c);
    if (e1 != ESP_OK) result = e1;
    else if (e2 != ESP_OK) result = e2;
    if (result == ESP_OK) s_last_sync_us = esp_timer_get_time();
    return result;
}

esp_err_t load(const Config& c) {
    Lock lock;
    esp_err_t err = load_locked(c);
    if (err == ESP_OK) standby_load_locked(c);  // best effort: the standby list is a separate file
    return err;
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
    n.standby = s_standby_loaded ? cJSON_GetArraySize(standby_books()) : 0;
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
    if (s_standby_loaded) {  // the member's save-for-later list (titles only; get_standby_list has the detail)
        cJSON* sb = cJSON_AddArrayToObject(o, "onHoldList");
        const cJSON* s;
        int shown = 0;
        cJSON_ArrayForEach(s, standby_books()) {
            if (shown++ >= 30) break;
            cJSON_AddItemToArray(sb, cJSON_CreateString(str(s, "Title").c_str()));
        }
    }
    // Books the member rated 4 or 5 (their favourites), capped so the prompt stays small.
    cJSON* favs = cJSON_AddArrayToObject(o, "favoriteBooks");
    const cJSON* h;
    int n = 0;
    cJSON_ArrayForEach(h, arr("ReadingHistory")) {
        const cJSON* rating = cJSON_GetObjectItemCaseSensitive(h, "Rating");
        if (!cJSON_IsNumber(rating) || rating->valueint < 4 || n >= 30) continue;
        cJSON* x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "title", str(h, "Title").c_str());
        std::string who = va::natural_author(str(h, "Author"));
        if (!who.empty()) cJSON_AddStringToObject(x, "author", who.c_str());
        cJSON_AddNumberToObject(x, "rating", rating->valueint);
        cJSON_AddItemToArray(favs, x);
        n++;
    }
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
        if (!str(e, "DateAdded").empty()) cJSON_AddStringToObject(x, "dateAdded", str(e, "DateAdded").c_str());
        const cJSON* removed = cJSON_GetObjectItemCaseSensitive(e, "DateRemoved");
        if (cJSON_IsString(removed)) cJSON_AddStringToObject(x, "dateRemoved", removed->valuestring);
        const cJSON* rating = cJSON_GetObjectItemCaseSensitive(e, "Rating");
        if (cJSON_IsNumber(rating)) cJSON_AddNumberToObject(x, "rating", rating->valueint);
        cJSON_AddItemToArray(out, x);
    }
    return print(out);
}

// The list entry for a book: by catalogue id when both have one, otherwise by exact title (ignoring case).
static cJSON* find_entry(const std::string& title, const std::string& bookshare_id) {
    cJSON* e;
    if (!bookshare_id.empty()) {
        cJSON_ArrayForEach(e, arr("ReadingHistory")) {
            if (str(e, "BookshareId") == bookshare_id) return e;
        }
    }
    if (!title.empty()) {
        cJSON_ArrayForEach(e, arr("ReadingHistory")) {
            if (lower(str(e, "Title")) == lower(title)) return e;
        }
    }
    return nullptr;
}

static int rating_of(const cJSON* e) {
    const cJSON* r = cJSON_GetObjectItemCaseSensitive(e, "Rating");
    return cJSON_IsNumber(r) ? r->valueint : 0;
}

static void set_rating(cJSON* e, int rating) {
    cJSON_DeleteItemFromObjectCaseSensitive(e, "Rating");
    cJSON_AddNumberToObject(e, "Rating", rating);
}

esp_err_t add_book(const Config& c, const std::string& title, const std::string& author, const std::string& bookshare_id,
                   bool favorite, bool* created) {
    bool made = false;
    std::string nat = va::natural_author(author);
    esp_err_t err = update(c, [&] {
        if (cJSON* e = find_entry(title, bookshare_id)) {
            made = false;
            if (favorite && rating_of(e) < 4) {
                set_rating(e, 5);
            } else if (!favorite && rating_of(e) >= 4) {
                cJSON_DeleteItemFromObjectCaseSensitive(e, "Rating");
                cJSON_AddNullToObject(e, "Rating");
            }
            if (str(e, "Author").empty() && !nat.empty()) {
                cJSON_DeleteItemFromObjectCaseSensitive(e, "Author");
                cJSON_AddStringToObject(e, "Author", nat.c_str());
            }
            if (str(e, "BookshareId").empty() && !bookshare_id.empty()) {
                cJSON_DeleteItemFromObjectCaseSensitive(e, "BookshareId");
                cJSON_AddStringToObject(e, "BookshareId", bookshare_id.c_str());
            }
            return;
        }
        made = true;
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "Title", title.c_str());
        if (nat.empty()) cJSON_AddNullToObject(e, "Author");
        else cJSON_AddStringToObject(e, "Author", nat.c_str());
        if (bookshare_id.empty()) cJSON_AddNullToObject(e, "BookshareId");
        else cJSON_AddStringToObject(e, "BookshareId", bookshare_id.c_str());
        // No DateAdded: it was never on the bookshelf. No DateRemoved either.
        cJSON_AddNullToObject(e, "DateRemoved");
        if (favorite) cJSON_AddNumberToObject(e, "Rating", 5);
        else cJSON_AddNullToObject(e, "Rating");
        cJSON_AddItemToArray(arr("ReadingHistory"), e);
    });
    if (created) *created = made;
    return err;
}

esp_err_t remove_book(const Config& c, const std::string& title, std::string* matched_title, int* matches) {
    Lock lock;
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    if (!s_loaded && load_locked(c) != ESP_OK) return ESP_FAIL;

    // Prefer an exact (case-insensitive) title; otherwise a partial match must be unique.
    std::vector<std::string> exact, partial;
    const cJSON* e;
    cJSON_ArrayForEach(e, arr("ReadingHistory")) {
        std::string t = str(e, "Title");
        if (lower(t) == lower(title)) exact.push_back(t);
        else if (contains_ci(t, title)) partial.push_back(t);
    }
    const std::vector<std::string>& hits = !exact.empty() ? exact : partial;
    if (matches) *matches = static_cast<int>(hits.size());
    if (hits.empty()) return ESP_ERR_NOT_FOUND;
    if (hits.size() > 1) return ESP_ERR_INVALID_SIZE;
    const std::string found = hits[0];
    if (matched_title) *matched_title = found;

    return update(c, [&] {
        cJSON* list = arr("ReadingHistory");
        int n = cJSON_GetArraySize(list);
        for (int i = 0; i < n; i++) {
            if (str(cJSON_GetArrayItem(list, i), "Title") == found) {
                cJSON_DeleteItemFromArray(list, i);
                return;
            }
        }
    });
}

std::string reading_profile(const va::Shelf& shelf) {
    Lock lock;
    struct Count {
        std::string display;
        int n = 0;
    };
    std::map<std::string, Count> authors;  // by lowercase natural name
    auto count_author = [&](const std::string& natural) {
        if (natural.empty()) return;
        Count& c = authors[lower(natural)];
        if (c.display.empty()) c.display = natural;
        c.n++;
    };

    std::set<std::string> seen_ids, seen_titles;  // a book on the shelf and in the history counts once
    cJSON* on_shelf = cJSON_CreateArray();
    for (const auto& b : shelf.books) {
        count_author(b.author);
        if (!b.bookshare_id.empty()) seen_ids.insert(b.bookshare_id);
        seen_titles.insert(lower(b.title));
        cJSON* x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "title", b.title.c_str());
        if (!b.author.empty()) cJSON_AddStringToObject(x, "author", b.author.c_str());
        cJSON_AddItemToArray(on_shelf, x);
    }

    // The books list: newest 60 entries by position, and the favourites (rated 4 or 5).
    const cJSON* list = arr("ReadingHistory");
    const int total = cJSON_GetArraySize(list);
    cJSON* known = cJSON_CreateArray();
    cJSON* favorites = cJSON_CreateArray();
    for (int i = 0; i < total; i++) {
        const cJSON* e = cJSON_GetArrayItem(list, i);
        std::string title = str(e, "Title"), id = str(e, "BookshareId");
        std::string who = va::natural_author(str(e, "Author"));
        bool on_shelf_now = (!id.empty() && seen_ids.count(id)) || seen_titles.count(lower(title));
        if (!on_shelf_now) count_author(who);  // shelf books were counted above
        const cJSON* r = cJSON_GetObjectItemCaseSensitive(e, "Rating");
        int rating = cJSON_IsNumber(r) ? r->valueint : 0;
        if (i >= total - 60) {
            cJSON* x = cJSON_CreateObject();
            cJSON_AddStringToObject(x, "title", title.c_str());
            if (!who.empty()) cJSON_AddStringToObject(x, "author", who.c_str());
            if (rating) cJSON_AddNumberToObject(x, "rating", rating);
            if (on_shelf_now) cJSON_AddBoolToObject(x, "onBookshelfNow", true);
            cJSON_AddItemToArray(known, x);
        }
        if (rating >= 4 && cJSON_GetArraySize(favorites) < 30) {
            cJSON* x = cJSON_CreateObject();
            cJSON_AddStringToObject(x, "title", title.c_str());
            if (!who.empty()) cJSON_AddStringToObject(x, "author", who.c_str());
            cJSON_AddNumberToObject(x, "rating", rating);
            cJSON_AddItemToArray(favorites, x);
        }
    }

    std::vector<Count> ranked;
    for (const auto& kv : authors) ranked.push_back(kv.second);
    std::sort(ranked.begin(), ranked.end(), [](const Count& a, const Count& b) {
        return a.n != b.n ? a.n > b.n : a.display < b.display;
    });
    cJSON* frequent = cJSON_CreateArray();
    for (size_t i = 0; i < ranked.size() && i < 8; i++) {
        cJSON* x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "author", ranked[i].display.c_str());
        cJSON_AddNumberToObject(x, "books", ranked[i].n);
        cJSON_AddItemToArray(frequent, x);
    }

    cJSON* fav_authors = cJSON_CreateArray();
    cJSON* other_authors = cJSON_CreateArray();
    const cJSON* a;
    cJSON_ArrayForEach(a, arr("PreferredAuthors")) {
        std::string who = va::natural_author(str(a, "AuthorName"));
        cJSON_AddItemToArray(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(a, "IsFavorite")) ? fav_authors : other_authors,
                             cJSON_CreateString(who.c_str()));
    }

    cJSON* o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "frequentAuthors", frequent);
    cJSON_AddItemToObject(o, "favoriteAuthors", fav_authors);
    cJSON_AddItemToObject(o, "otherAuthorsTheyKnow", other_authors);
    cJSON_AddItemToObject(o, "favoriteBooks", favorites);
    cJSON_AddItemToObject(o, "onBookshelfNow", on_shelf);
    cJSON_AddItemToObject(o, "booksList", known);
    cJSON_AddNumberToObject(o, "booksListSize", total);
    cJSON_AddNumberToObject(o, "loanSlotsUsed", shelf.loan_count);
    cJSON_AddNumberToObject(o, "loanSlotsTotal", va::kLoanCap);
    return print(o);
}

esp_err_t standby_list(const Config& c, std::string* json) {
    Lock lock;
    esp_err_t err = standby_ensure_loaded(c);
    if (err != ESP_OK) return err;
    cJSON* out = cJSON_CreateArray();
    const cJSON* e;
    cJSON_ArrayForEach(e, standby_books()) {
        cJSON* x = cJSON_CreateObject();
        cJSON_AddStringToObject(x, "title", str(e, "Title").c_str());
        std::string who = va::natural_author(str(e, "Author"));
        if (!who.empty()) cJSON_AddStringToObject(x, "author", who.c_str());
        if (!str(e, "BookshareId").empty()) cJSON_AddStringToObject(x, "bookshareId", str(e, "BookshareId").c_str());
        if (!str(e, "Note").empty()) cJSON_AddStringToObject(x, "note", str(e, "Note").c_str());
        if (!str(e, "DateAdded").empty()) cJSON_AddStringToObject(x, "dateAdded", str(e, "DateAdded").c_str());
        cJSON_AddItemToArray(out, x);
    }
    if (json) *json = print(out);
    else cJSON_Delete(out);
    return ESP_OK;
}

esp_err_t standby_add_many(const Config& c, const std::vector<StandbyEntry>& entries, int* created, int* updated) {
    Lock lock;
    int made = 0, changed = 0;
    esp_err_t err = standby_update(c, [&] {
        made = changed = 0;  // the mutation may run twice (after an ETag conflict)
        for (const StandbyEntry& in : entries) {
            if (in.title.empty()) continue;
            std::string nat = va::natural_author(in.author);
            if (cJSON* e = standby_find(in.title, in.bookshare_id)) {  // already there: fill in what was missing
                changed++;
                if (str(e, "Author").empty() && !nat.empty()) {
                    cJSON_DeleteItemFromObjectCaseSensitive(e, "Author");
                    cJSON_AddStringToObject(e, "Author", nat.c_str());
                }
                if (str(e, "BookshareId").empty() && !in.bookshare_id.empty()) {
                    cJSON_DeleteItemFromObjectCaseSensitive(e, "BookshareId");
                    cJSON_AddStringToObject(e, "BookshareId", in.bookshare_id.c_str());
                }
                if (!in.note.empty()) {
                    cJSON_DeleteItemFromObjectCaseSensitive(e, "Note");
                    cJSON_AddStringToObject(e, "Note", in.note.c_str());
                }
                continue;
            }
            made++;
            cJSON* e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "Title", in.title.c_str());
            if (!nat.empty()) cJSON_AddStringToObject(e, "Author", nat.c_str());
            if (!in.bookshare_id.empty()) cJSON_AddStringToObject(e, "BookshareId", in.bookshare_id.c_str());
            if (!in.note.empty()) cJSON_AddStringToObject(e, "Note", in.note.c_str());
            std::string when = now_iso();
            if (!when.empty()) cJSON_AddStringToObject(e, "DateAdded", when.c_str());
            cJSON_AddItemToArray(standby_books(), e);
        }
    });
    if (created) *created = made;
    if (updated) *updated = changed;
    return err;
}

esp_err_t standby_add(const Config& c, const std::string& title, const std::string& author,
                      const std::string& bookshare_id, const std::string& note, bool* created) {
    int made = 0, changed = 0;
    esp_err_t err = standby_add_many(c, {StandbyEntry{title, author, bookshare_id, note}}, &made, &changed);
    if (created) *created = made > 0;
    return err;
}

esp_err_t standby_remove(const Config& c, const std::string& title, std::string* matched_title, int* matches) {
    Lock lock;
    esp_err_t err = standby_ensure_loaded(c);
    if (err != ESP_OK) return err;
    std::vector<std::string> exact, partial;
    const cJSON* e;
    cJSON_ArrayForEach(e, standby_books()) {
        std::string t = str(e, "Title");
        if (lower(t) == lower(title)) exact.push_back(t);
        else if (contains_ci(t, title)) partial.push_back(t);
    }
    const std::vector<std::string>& hits = !exact.empty() ? exact : partial;
    if (matches) *matches = static_cast<int>(hits.size());
    if (hits.empty()) return ESP_ERR_NOT_FOUND;
    if (hits.size() > 1) return ESP_ERR_INVALID_SIZE;
    const std::string found = hits[0];
    if (matched_title) *matched_title = found;
    return standby_update(c, [&] { standby_delete(found); });
}

esp_err_t standby_take(const Config& c, const std::string& bookshare_id, const std::string& title,
                       const std::string& entry_title, std::string* removed_title) {
    Lock lock;
    esp_err_t err = standby_ensure_loaded(c);
    if (err != ESP_OK) return err;
    const cJSON* e = !entry_title.empty() ? standby_find(entry_title, "") : standby_find(title, bookshare_id);
    if (!e) return ESP_ERR_NOT_FOUND;
    const std::string found = str(e, "Title");
    if (removed_title) *removed_title = found;
    return standby_update(c, [&] { standby_delete(found); });
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

esp_err_t remove_author(const Config& c, const std::string& name, std::string* matched_name, int* matches) {
    Lock lock;
    if (c.memory_url.empty()) return ESP_ERR_INVALID_STATE;
    if (!s_loaded && load_locked(c) != ESP_OK) return ESP_FAIL;

    // Prefer an exact match on the natural name; otherwise a partial match must be unique.
    const std::string want = author_key(name);
    std::vector<std::string> exact, partial;
    const cJSON* a;
    cJSON_ArrayForEach(a, arr("PreferredAuthors")) {
        std::string nat = va::natural_author(str(a, "AuthorName"));
        if (lower(nat) == want) exact.push_back(nat);
        else if (contains_ci(nat, name) || contains_ci(name, nat)) partial.push_back(nat);
    }
    const std::vector<std::string>& hits = !exact.empty() ? exact : partial;
    if (matches) *matches = static_cast<int>(hits.size());
    if (hits.empty()) return ESP_ERR_NOT_FOUND;
    if (hits.size() > 1) return ESP_ERR_INVALID_SIZE;
    const std::string found = hits[0];
    if (matched_name) *matched_name = found;

    return update(c, [&] {
        cJSON* list = arr("PreferredAuthors");
        int n = cJSON_GetArraySize(list);
        for (int i = 0; i < n; i++) {
            if (lower(va::natural_author(str(cJSON_GetArrayItem(list, i), "AuthorName"))) == lower(found)) {
                cJSON_DeleteItemFromArray(list, i);
                return;
            }
        }
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

// Splits "Tom Clancy, Steve Pieczenik" or "A and B" into single names (natural names contain no commas).
static std::vector<std::string> split_authors(const std::string& names) {
    std::vector<std::string> out;
    std::string rest = names;
    while (!rest.empty()) {
        size_t comma = rest.find(", "), and_pos = rest.find(" and ");
        size_t cut = std::min(comma, and_pos);
        std::string one = rest.substr(0, cut);
        size_t skip = cut == std::string::npos ? rest.size() : (cut == comma ? 2 : 5);
        rest = cut == std::string::npos ? "" : rest.substr(cut + skip);
        while (!one.empty() && one.front() == ' ') one.erase(one.begin());
        while (!one.empty() && one.back() == ' ') one.pop_back();
        if (!one.empty()) out.push_back(one);
    }
    return out;
}

esp_err_t log_added(const Config& c, const std::string& title, const std::string& author, const std::string& bookshare_id,
                    bool* author_added) {
    bool added_author = false;
    esp_err_t err = update(c, [&] {
        added_author = false;
        std::string when = now_iso();
        if (cJSON* existing = find_entry(title, bookshare_id)) {
            // Already on the list (for example a favourite added earlier, or borrowed before): reuse the
            // entry, with a fresh add date, no remove date, and its rating and author kept.
            cJSON_DeleteItemFromObjectCaseSensitive(existing, "DateAdded");
            if (!when.empty()) cJSON_AddStringToObject(existing, "DateAdded", when.c_str());
            cJSON_DeleteItemFromObjectCaseSensitive(existing, "DateRemoved");
            cJSON_AddNullToObject(existing, "DateRemoved");
            if (str(existing, "Author").empty() && !author.empty()) {
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "Author");
                cJSON_AddStringToObject(existing, "Author", author.c_str());
            }
            if (str(existing, "BookshareId").empty() && !bookshare_id.empty()) {
                cJSON_DeleteItemFromObjectCaseSensitive(existing, "BookshareId");
                cJSON_AddStringToObject(existing, "BookshareId", bookshare_id.c_str());
            }
        } else {
            cJSON* e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "Title", title.c_str());
            if (author.empty()) cJSON_AddNullToObject(e, "Author");
            else cJSON_AddStringToObject(e, "Author", author.c_str());
            if (bookshare_id.empty()) cJSON_AddNullToObject(e, "BookshareId");
            else cJSON_AddStringToObject(e, "BookshareId", bookshare_id.c_str());
            if (!when.empty()) cJSON_AddStringToObject(e, "DateAdded", when.c_str());
            cJSON_AddNullToObject(e, "DateRemoved");
            cJSON_AddNullToObject(e, "Rating");
            cJSON_AddItemToArray(arr("ReadingHistory"), e);
        }

        // Adding a book puts its author on the authors list too, if they are not already there.
        for (const std::string& who : split_authors(va::natural_author(author))) {
            bool known = false;
            cJSON* a;
            cJSON_ArrayForEach(a, arr("PreferredAuthors")) {
                if (author_key(str(a, "AuthorName")) == lower(who)) known = true;
            }
            if (known) continue;
            cJSON* x = cJSON_CreateObject();
            cJSON_AddStringToObject(x, "AuthorName", who.c_str());
            cJSON_AddBoolToObject(x, "IsFavorite", false);
            if (!when.empty()) cJSON_AddStringToObject(x, "DateAdded", when.c_str());
            cJSON_AddItemToArray(arr("PreferredAuthors"), x);
            added_author = true;
        }
    });
    if (author_added) *author_added = added_author;
    return err;
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
