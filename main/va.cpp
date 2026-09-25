#include "va.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <memory>
#include <strings.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

static const char* TAG = "va";

namespace {

constexpr const char* kBase = "https://my.visionaustralia.org";
constexpr const char* kUserAgent = "BookBook/0.1 (personal accessibility assistant; contact via VA account)";
constexpr size_t kMaxBody = 400 * 1024;
constexpr int64_t kMinGapUs = 300 * 1000;  // polite pacing between calls

struct Response {
    int status = 0;
    std::string body;
    std::string content_type;
    std::string location;
    bool truncated = false;
};

esp_http_client_handle_t s_client;
std::map<std::string, std::string> s_cookies;
std::string s_user, s_password;
bool s_logged_in;
int64_t s_last_call_us;
bool s_xhr;  // send X-Requested-With: XMLHttpRequest (what the site's own AJAX calls send)

struct Lock {
    static SemaphoreHandle_t handle() {
        static SemaphoreHandle_t h = xSemaphoreCreateRecursiveMutex();
        return h;
    }
    Lock() { xSemaphoreTakeRecursive(handle(), portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(handle()); }
};

void store_cookie(const char* set_cookie) {
    std::string c = set_cookie;
    size_t semi = c.find(';');
    std::string pair = c.substr(0, semi);
    size_t eq = pair.find('=');
    if (eq == std::string::npos || eq == 0) return;
    std::string name = pair.substr(0, eq), value = pair.substr(eq + 1);
    bool expired = value.empty() || value == "deleted" || c.find("Max-Age=0") != std::string::npos;
    if (expired) {
        s_cookies.erase(name);
    } else {
        s_cookies[name] = value;
    }
}

std::string cookie_header() {
    std::string h;
    for (const auto& kv : s_cookies) {
        if (!h.empty()) h += "; ";
        h += kv.first + "=" + kv.second;
    }
    return h;
}

esp_err_t on_http_event(esp_http_client_event_t* e) {
    auto* r = static_cast<Response*>(e->user_data);
    if (!r) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(e->header_key, "Set-Cookie") == 0) {
            store_cookie(e->header_value);
        } else if (strcasecmp(e->header_key, "Content-Type") == 0) {
            r->content_type = e->header_value;
        } else if (strcasecmp(e->header_key, "Location") == 0) {
            r->location = e->header_value;
        }
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        if (r->body.size() + e->data_len <= kMaxBody) {
            r->body.append(static_cast<const char*>(e->data), e->data_len);
        } else {
            r->truncated = true;
        }
    }
    return ESP_OK;
}

void close_client() {
    if (s_client) {
        esp_http_client_cleanup(s_client);
        s_client = nullptr;
    }
}

bool open_client() {
    if (s_client) return true;
    esp_http_client_config_t cfg = {};
    cfg.url = kBase;
    cfg.event_handler = on_http_event;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 20000;
    cfg.keep_alive_enable = true;
    cfg.disable_auto_redirect = true;  // a redirect to the login page means the session expired
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;
    s_client = esp_http_client_init(&cfg);
    return s_client != nullptr;
}

// One HTTP request with pacing, our own cookie jar, and a single reconnect retry.
esp_err_t request(esp_http_client_method_t method, const std::string& path, const char* content_type,
                  const std::string* body, const char* csrf, Response& out) {
    for (int attempt = 0; attempt < 2; attempt++) {
        int64_t wait_us = s_last_call_us + kMinGapUs - esp_timer_get_time();
        if (wait_us > 0) vTaskDelay(pdMS_TO_TICKS((wait_us + 999) / 1000));

        if (!open_client()) return ESP_ERR_NO_MEM;
        out = Response();
        std::string url = kBase + path;
        esp_http_client_set_url(s_client, url.c_str());
        esp_http_client_set_method(s_client, method);
        esp_http_client_set_user_data(s_client, &out);
        esp_http_client_set_header(s_client, "User-Agent", kUserAgent);
        esp_http_client_set_header(s_client, "Accept", "application/json, text/html;q=0.8, */*;q=0.5");
        std::string cookies = cookie_header();
        if (cookies.empty()) esp_http_client_delete_header(s_client, "Cookie");
        else esp_http_client_set_header(s_client, "Cookie", cookies.c_str());
        if (content_type) esp_http_client_set_header(s_client, "Content-Type", content_type);
        else esp_http_client_delete_header(s_client, "Content-Type");
        if (csrf) esp_http_client_set_header(s_client, "X-CSRF-Token", csrf);
        else esp_http_client_delete_header(s_client, "X-CSRF-Token");
        if (s_xhr) esp_http_client_set_header(s_client, "X-Requested-With", "XMLHttpRequest");
        else esp_http_client_delete_header(s_client, "X-Requested-With");
        if (body) esp_http_client_set_post_field(s_client, body->data(), static_cast<int>(body->size()));
        else esp_http_client_set_post_field(s_client, nullptr, 0);

        int64_t t_start = esp_timer_get_time();
        esp_err_t err = esp_http_client_perform(s_client);
        s_last_call_us = esp_timer_get_time();
        if (err == ESP_OK) {
            out.status = esp_http_client_get_status_code(s_client);
            ESP_LOGI(TAG, "%s %s: HTTP %d, %u bytes, %d ms", method == HTTP_METHOD_POST ? "POST" : "GET", path.substr(0, 48).c_str(),
                     out.status, static_cast<unsigned>(out.body.size()), static_cast<int>((s_last_call_us - t_start) / 1000));
            return out.truncated ? ESP_ERR_NO_MEM : ESP_OK;
        }
        ESP_LOGW(TAG, "request %s failed: %s%s", path.c_str(), esp_err_to_name(err),
                 attempt == 0 ? " (reconnecting)" : "");
        close_client();  // stale keep-alive connection or network blip: retry once on a fresh one
    }
    return ESP_FAIL;
}

// GET that follows up to 5 same-site redirects (needed for the login handshake's final step).
esp_err_t get_following(const std::string& first_path, Response& out) {
    std::string path = first_path;
    for (int hop = 0; hop < 6; hop++) {
        esp_err_t err = request(HTTP_METHOD_GET, path, nullptr, nullptr, nullptr, out);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "GET %s -> HTTP %d%s%s", path.c_str(), out.status, out.location.empty() ? "" : " -> ",
                 out.location.c_str());
        if (out.status < 300 || out.status >= 400 || out.location.empty()) return ESP_OK;
        std::string loc = out.location;
        if (loc.rfind(kBase, 0) == 0) loc.erase(0, strlen(kBase));
        if (!loc.empty() && loc[0] == '?') loc.insert(0, "/");  // "https://host?x=1" has an empty path
        if (loc.empty() || loc[0] != '/') return ESP_ERR_INVALID_RESPONSE;  // off-site redirect: refuse
        path = loc;
    }
    return ESP_ERR_INVALID_STATE;
}

std::string base64(const std::string& s) {
    size_t olen = 0;
    mbedtls_base64_encode(nullptr, 0, &olen, reinterpret_cast<const unsigned char*>(s.data()), s.size());
    std::string out(olen, '\0');
    mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &olen,
                          reinterpret_cast<const unsigned char*>(s.data()), s.size());
    out.resize(olen);
    return out;
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

// String field, tolerating VA's habit of sending numbers where strings are expected.
std::string jstr(const cJSON* obj, const char* key) {
    const cJSON* it = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    if (cJSON_IsString(it) && it->valuestring) return it->valuestring;
    if (cJSON_IsNumber(it)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", it->valuedouble);
        return buf;
    }
    return "";
}

int jint(const cJSON* obj, const char* key) {
    const cJSON* it = obj ? cJSON_GetObjectItemCaseSensitive(obj, key) : nullptr;
    return cJSON_IsNumber(it) ? it->valueint : 0;
}

bool is_json(const Response& r) { return r.content_type.find("json") != std::string::npos; }

// Authenticated JSON call. A non-JSON answer (redirect/HTML login page) means the session expired:
// log in again once, then retry.
esp_err_t call_json(esp_http_client_method_t method, const std::string& path, const char* content_type,
                    const std::string* body, cJSON** root) {
    for (int attempt = 0; attempt < 2; attempt++) {
        Response r;
        esp_err_t err = request(method, path, content_type, body, nullptr, r);
        if (err != ESP_OK) return err;
        if ((r.status >= 300 && r.status < 400) || !is_json(r)) {
            ESP_LOGW(TAG, "%s: HTTP %d, content-type '%s', not JSON (session expired?): %.160s", path.c_str(), r.status,
                     r.content_type.c_str(), r.body.c_str());
            s_logged_in = false;
            if (attempt == 0 && !s_user.empty()) {
                std::string why;
                if (va::login(s_user, s_password, &why) == ESP_OK) continue;
            }
            return ESP_ERR_INVALID_STATE;
        }
        if (r.status != 200) {
            ESP_LOGW(TAG, "%s: HTTP %d: %.160s", path.c_str(), r.status, r.body.c_str());
            return ESP_FAIL;
        }
        *root = cJSON_ParseWithLength(r.body.data(), r.body.size());
        return *root ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_FAIL;
}

struct JsonDeleter {
    void operator()(cJSON* p) const { cJSON_Delete(p); }
};
using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

std::string trim(std::string s) {
    while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string one_author(std::string raw) {
    raw = trim(raw);
    if (raw.rfind("By ", 0) == 0) raw = trim(raw.substr(3));
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t comma = raw.find(',', start);
        std::string p = trim(raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        // Dates such as "1960-" or "1931-2020" contain digits: drop them.
        if (!p.empty() && p.find_first_of("0123456789") == std::string::npos) parts.push_back(p);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parts.size() == 2) return parts[1] + " " + parts[0];
    std::string joined;
    for (const auto& p : parts) joined += (joined.empty() ? "" : ", ") + p;
    return joined;
}

void parse_shelf_item(const cJSON* it, va::ShelfItem& out) {
    const cJSON* book = cJSON_GetObjectItemCaseSensitive(it, "book");
    out.active_title_id = jstr(it, "activeTitleId");
    out.title = jstr(it, "title");
    if (out.title.empty()) out.title = jstr(book, "title");
    out.author = va::natural_author(jstr(book, "authorBy"));
    out.bookshare_id = jstr(book, "bookshareId");
    out.format = jstr(cJSON_GetObjectItemCaseSensitive(it, "format"), "name");
    out.status = jstr(cJSON_GetObjectItemCaseSensitive(it, "status"), "key");
    out.date_added = jstr(it, "dateAdded");
}

}  // namespace

namespace va {

std::string natural_author(const std::string& catalogue_name) {
    std::string out;
    size_t start = 0;
    while (start <= catalogue_name.size()) {
        size_t semi = catalogue_name.find(';', start);
        std::string one = one_author(catalogue_name.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
        if (!one.empty()) out += (out.empty() ? "" : " and ") + one;
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return out;
}

bool logged_in() { return s_logged_in; }

// Mirrors dodp_auth/js/login.js: GET the login page for a CSRF token, POST base64 credentials as
// JSON with that token, then GET /dodp-auth/api/authorize to establish the real session.
esp_err_t login(const std::string& user, const std::string& password, std::string* error) {
    Lock lock;
    s_user = user;
    s_password = password;
    s_logged_in = false;
    s_cookies.clear();
    auto fail = [&](const char* why, esp_err_t code) {
        if (error) *error = why;
        ESP_LOGW(TAG, "login failed: %s", why);
        return code;
    };
    if (user.empty() || password.empty()) return fail("no library login saved", ESP_ERR_INVALID_ARG);

    Response page;
    esp_err_t err = request(HTTP_METHOD_GET, "/library/login", nullptr, nullptr, nullptr, page);
    if (err != ESP_OK || page.status != 200) return fail("could not reach the library website", ESP_FAIL);

    size_t at = page.body.find("csrf-token");
    size_t v = at == std::string::npos ? at : page.body.find("value=", at);
    if (v == std::string::npos || v + 7 > page.body.size()) return fail("no CSRF token on login page", ESP_FAIL);
    char quote = page.body[v + 6];
    size_t end = page.body.find(quote, v + 7);
    if ((quote != '"' && quote != '\'') || end == std::string::npos) return fail("no CSRF token on login page", ESP_FAIL);
    std::string csrf = page.body.substr(v + 7, end - (v + 7));

    JsonPtr payload(cJSON_CreateObject());
    cJSON_AddStringToObject(payload.get(), "email", base64(user).c_str());
    cJSON_AddStringToObject(payload.get(), "password", base64(password).c_str());
    char* raw = cJSON_PrintUnformatted(payload.get());
    std::string body = raw ? raw : "";
    cJSON_free(raw);

    Response auth;
    err = request(HTTP_METHOD_POST, "/dodp-auth/api/authenticate", "application/json", &body, csrf.c_str(), auth);
    if (err != ESP_OK || auth.status != 200) return fail("library login request failed", ESP_FAIL);
    JsonPtr result(cJSON_ParseWithLength(auth.body.data(), auth.body.size()));
    std::string code = jstr(result.get(), "errorCode"), msg = jstr(result.get(), "errorMessage");
    if (!code.empty() || !msg.empty()) {
        if (msg == "PasswordMustBeSet") return fail("password must be set on the website first", ESP_ERR_INVALID_STATE);
        return fail("library rejected the login", ESP_ERR_INVALID_STATE);
    }

    Response authorize;
    err = get_following("/dodp-auth/api/authorize", authorize);
    if (err != ESP_OK || authorize.status != 200) {
        ESP_LOGW(TAG, "authorize ended with err=%s HTTP %d", esp_err_to_name(err), authorize.status);
        return fail("library session was not established", ESP_FAIL);
    }

    s_logged_in = true;
    ESP_LOGI(TAG, "logged in");
    return ESP_OK;
}

esp_err_t search(const std::string& keyword, SearchResult& out, int limit, const char* type) {
    Lock lock;
    out = SearchResult();
    std::string form = "keyword=" + url_encode(keyword) + "&type=" + url_encode(type) +
                       "&limit=" + std::to_string(limit) + "&format=";
    cJSON* raw = nullptr;
    esp_err_t err = call_json(HTTP_METHOD_POST, "/library/quick-search", "application/x-www-form-urlencoded", &form, &raw);
    if (err != ESP_OK) return err;
    JsonPtr root(raw);
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
    const cJSON* tab = cJSON_GetObjectItemCaseSensitive(data, "bookTab");
    out.total = jint(tab, "total");
    const cJSON* item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(tab, "listArticle")) {
        BookHit h;
        h.bookshare_id = jstr(item, "bookshareId");
        h.title = jstr(item, "title");
        const cJSON* a;
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(item, "authors")) {
            std::string who = va::natural_author(jstr(a, "displayName"));
            if (who.empty()) continue;
            if (!h.authors.empty()) h.authors += ", ";
            h.authors += who;
        }
        h.status = jstr(cJSON_GetObjectItemCaseSensitive(item, "status"), "key");
        const cJSON* f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(item, "formats")) {
            std::string id = jstr(f, "formatId");
            if (!id.empty()) h.formats.push_back(id);
        }
        out.items.push_back(std::move(h));
    }
    return ESP_OK;
}

esp_err_t bookshelf(Shelf& out) {
    Lock lock;
    out = Shelf();
    cJSON* raw = nullptr;
    esp_err_t err = call_json(HTTP_METHOD_GET,
                              "/library/my-library/my-bookshelf?tabChild=tab-book&limit=20&currentPage=1&sortOrder=dateAdded&direction=desc",
                              nullptr, nullptr, &raw);
    if (err != ESP_OK) return err;
    JsonPtr root(raw);
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
    out.loan_count = jint(data, "totalBookAndMusicBraille");
    out.periodical_count = jint(data, "totalPeriodical");
    const cJSON* item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(data, "books")) {
        ShelfItem s;
        parse_shelf_item(item, s);
        out.books.push_back(std::move(s));
    }
    return ESP_OK;
}

namespace {
std::string safe_type(const std::string& t) { return (t == "music" || t == "periodical") ? t : "book"; }

// Ids and formats go into a URL path: keep only characters that cannot change its meaning.
std::string safe_segment(const std::string& s) {
    std::string o;
    for (char c : s) if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') o += c;
    return o;
}

// The portal answers with JSON; keep a short readable version for logs and the tool result.
esp_err_t write_call(esp_http_client_method_t method, const std::string& path, const char* content_type,
                     const std::string* body, std::string* reply) {
    std::string empty;
    if (method == HTTP_METHOD_POST && !body) body = &empty;
    cJSON* raw = nullptr;
    s_xhr = true;
    esp_err_t err = call_json(method, path, content_type, body, &raw);
    s_xhr = false;
    if (err != ESP_OK) return err;
    JsonPtr root(raw);
    char* s = cJSON_PrintUnformatted(root.get());
    std::string text = s ? s : "";
    cJSON_free(s);
    if (text.size() > 300) text.resize(300);
    ESP_LOGI(TAG, "%s -> %s", path.c_str(), text.c_str());
    if (reply) *reply = text;
    return ESP_OK;
}
}  // namespace

esp_err_t add_to_bookshelf(const std::string& bookshare_id, const std::string& format, const std::string& type,
                           std::string* reply) {
    Lock lock;
    std::string id = safe_segment(bookshare_id), fmt = safe_segment(format);
    if (id.empty() || fmt.empty()) return ESP_ERR_INVALID_ARG;
    return write_call(HTTP_METHOD_GET, "/library/my-bookshelf/add/" + id + "/" + fmt + "?type=" + safe_type(type), nullptr,
                      nullptr, reply);
}

// The site's bookshelf page removes a book with its checkbox + "Remove Selected" button, which POSTs
// /library/my-bookshelf/remove/all with book_active_title_ids=<activeTitleId> (form-encoded, one request
// per ticked book). The per-item GET /library/my-bookshelf/remove/{type}/{id} that Bookworm traced is
// only used for magazine issues, so it does not work for books. (my-bookshelf-tab.js, 2026-09.)
esp_err_t remove_from_bookshelf(const std::string& active_title_id, const std::string& type, std::string* reply) {
    Lock lock;
    if (type != "book") return ESP_ERR_NOT_SUPPORTED;  // music and periodicals use other calls: not verified
    std::string id = safe_segment(active_title_id);
    if (id.empty()) return ESP_ERR_INVALID_ARG;
    std::string body = "book_active_title_ids=" + id;
    return write_call(HTTP_METHOD_POST, "/library/my-bookshelf/remove/all", "application/x-www-form-urlencoded; charset=UTF-8",
                      &body, reply);
}

esp_err_t add_to_request_list(const std::string& bookshare_id, std::string* reply) {
    Lock lock;
    std::string id = safe_segment(bookshare_id);
    if (id.empty()) return ESP_ERR_INVALID_ARG;
    return write_call(HTTP_METHOD_POST, "/library/request-list/add/" + id, nullptr, nullptr, reply);
}

esp_err_t request_list(std::vector<ShelfItem>& out, int* total) {
    Lock lock;
    out.clear();
    cJSON* raw = nullptr;
    esp_err_t err = call_json(HTTP_METHOD_GET, "/library/request-list?sortOrder=dateAdded&direction=asc&limit=10&currentPage=1",
                              nullptr, nullptr, &raw);
    if (err != ESP_OK) return err;
    JsonPtr root(raw);
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root.get(), "data");
    if (total) *total = jint(data, "totalRequestList");
    const cJSON* item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(data, "requestList")) {
        ShelfItem s;
        parse_shelf_item(item, s);
        out.push_back(std::move(s));
    }
    return ESP_OK;
}

}  // namespace va
