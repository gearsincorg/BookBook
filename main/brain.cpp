#include "brain.h"

#include <memory>
#include <utility>
#include <vector>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "va.h"

static const char* TAG = "brain";

namespace {

constexpr int kMaxToolRoundTrips = 5;
constexpr int kMaxMessages = 40;                       // history cap (trimmed at turn boundaries)
constexpr int64_t kIdleResetUs = 10LL * 60 * 1000000;  // forget the conversation after 10 idle minutes
constexpr size_t kMaxResponseBytes = 96 * 1024;

// Bookworm's persona rules, trimmed to what this build can actually do and tuned for speech.
const char kSystemPrompt[] =
    "You are BookBook, a voice librarian for a vision-impaired member of the Vision Australia Library. "
    "Everything you say is spoken aloud by a text-to-speech voice, and the member talks to you by holding "
    "a button, so what you receive is speech recognition and may be slightly wrong. There is no screen.\n\n"
    "Rules:\n"
    "1. Keep replies short and natural, usually one to three sentences. No lists, bullet points, markdown, "
    "symbols, or anything that only makes sense visually.\n"
    "2. Never read out a long list. When a search or the bookshelf returns many items, group them by author "
    "or series and summarise in a sentence or two, then offer a next step. Reading out a shelf of up to about "
    "five books by title and author is fine.\n"
    "3. Ask at most one clarifying question at a time.\n"
    "4. The library catalogue only searches by title, author or series, never by subject or theme. For "
    "\"something like X\" requests, first think of specific titles or authors from your own knowledge, then "
    "use search_library to check what is actually available.\n"
    "5. search_library results include moreResultsExist. When it is true, do not imply you have heard the "
    "full set; say there are more and offer to narrow by author, series or era.\n"
    "6. Speech recognition may mishear names, for example Cornwall for Cornwell. If a name looks garbled, "
    "try the most likely intended author or title.\n"
    "7. Say author names in natural order, for example Tom Clancy. Never say ids, and never mention tool "
    "names.\n"
    "8. Answer only what was asked. Do not volunteer counts, free space, other shelves or summaries, and "
    "never read out status values such as READY_FOR_DOWNLOAD; a title that is on the shelf is simply there.\n"
    "9. You can search the catalogue, list the bookshelf and the request list, add a title to the bookshelf "
    "or to the request list, and remove a title from the bookshelf. You cannot yet subscribe to periodicals, "
    "remove from the request list, or remember preferences between sessions. If asked for one of those, say "
    "so in one short sentence.\n"
    "10. Adding to the bookshelf or the request list needs no confirmation: just do it, then say it is done in "
    "one short sentence. Removing from the bookshelf is destructive: first say which book you would remove and "
    "ask whether to go ahead, and only call the remove tool after the member clearly says yes in their next "
    "message.\n"
    "11. When adding to the bookshelf, use a format from that title's formats list, preferring "
    "DAISY_Audio_Human, otherwise another audio format. If the bookshelf is full (no free slots), offer to add "
    "the title to the request list instead.\n"
    "12. If a tool result says dryRun, the change was only pretended (practice mode). Tell the member it was a "
    "practice run and that nothing on their real library account changed.\n";

const char kToolsJson[] = R"JSON([
 {"name":"search_library",
  "description":"Search the Vision Australia Library catalogue by keyword. Only matches title, author or series title, never subject or synopsis. For thematic requests, propose specific candidate titles or authors yourself first, then call this to check availability.",
  "input_schema":{"type":"object","properties":{
    "keyword":{"type":"string","description":"Search keywords (title, author, or series)."},
    "type":{"type":"string","enum":["Book","Picture Book","Magazine","Newspaper","Podcast","Music"],"description":"Item type to search. Defaults to Book."}},
   "required":["keyword"]}},
 {"name":"get_bookshelf",
  "description":"Get the current bookshelf: everything on loan, including how many of the 20 book/music loan slots are used.",
  "input_schema":{"type":"object","properties":{}}},
 {"name":"add_to_bookshelf",
  "description":"Add a title to the bookshelf (borrows it; uses one of the 20 loan slots). Do this straight away when asked, no confirmation needed. Use the bookshareId and a formatId from that title's search result.",
  "input_schema":{"type":"object","properties":{
    "bookshareId":{"type":"string","description":"The catalogue id from a search result."},
    "format":{"type":"string","description":"A formatId from the search result's formats list, e.g. DAISY_Audio_Human."},
    "title":{"type":"string","description":"The title, from the search result."},
    "type":{"type":"string","enum":["book","music","periodical"],"description":"Defaults to book."}},
   "required":["bookshareId","format","title"]}},
 {"name":"remove_from_bookshelf",
  "description":"Remove a title from the bookshelf, freeing a loan slot. Destructive: always say which title you would remove and get an explicit yes from the member in a prior message before calling this.",
  "input_schema":{"type":"object","properties":{
    "activeTitleId":{"type":"string","description":"The activeTitleId from get_bookshelf (not the bookshareId)."},
    "type":{"type":"string","enum":["book","music","periodical"],"description":"Defaults to book."}},
   "required":["activeTitleId"]}},
 {"name":"add_to_request_list",
  "description":"Add a title to the request list (no loan limit). Do this straight away when asked, no confirmation needed.",
  "input_schema":{"type":"object","properties":{
    "bookshareId":{"type":"string","description":"The catalogue id from a search result."}},
   "required":["bookshareId"]}},
 {"name":"get_request_list",
  "description":"Get the request list: titles saved to read later, which move to the bookshelf when a loan slot frees up.",
  "input_schema":{"type":"object","properties":{}}}
])JSON";

// ---- state -------------------------------------------------------------------------------------

struct Lock {
    static SemaphoreHandle_t handle() {
        static SemaphoreHandle_t h = xSemaphoreCreateRecursiveMutex();
        return h;
    }
    Lock() { xSemaphoreTakeRecursive(handle(), portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(handle()); }
};

struct JsonDeleter {
    void operator()(cJSON* p) const { cJSON_Delete(p); }
};
using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

cJSON* s_messages;            // conversation so far (array of {role, content})
cJSON* s_tools;               // parsed once
int64_t s_last_turn_us;
esp_http_client_handle_t s_client;

// ---- HTTP to the Messages API ------------------------------------------------------------------

struct Response {
    int status = 0;
    std::string body;
};

esp_err_t on_http_event(esp_http_client_event_t* e) {
    auto* r = static_cast<Response*>(e->user_data);
    if (r && e->event_id == HTTP_EVENT_ON_DATA && r->body.size() + e->data_len <= kMaxResponseBytes) {
        r->body.append(static_cast<const char*>(e->data), e->data_len);
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
    cfg.url = "https://api.anthropic.com/v1/messages";
    cfg.method = HTTP_METHOD_POST;
    cfg.event_handler = on_http_event;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 60000;
    cfg.keep_alive_enable = true;  // later round trips in a turn skip the ~0.7 s TLS handshake
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    s_client = esp_http_client_init(&cfg);
    return s_client != nullptr;
}

esp_err_t call_claude(const Config& c, const std::string& body, Response& out) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!open_client()) return ESP_ERR_NO_MEM;
        out = Response();
        esp_http_client_set_user_data(s_client, &out);
        esp_http_client_set_header(s_client, "x-api-key", c.anthropic_key.c_str());
        esp_http_client_set_header(s_client, "anthropic-version", "2023-06-01");
        esp_http_client_set_header(s_client, "content-type", "application/json");
        esp_http_client_set_post_field(s_client, body.data(), static_cast<int>(body.size()));
        esp_err_t err = esp_http_client_perform(s_client);
        if (err == ESP_OK) {
            out.status = esp_http_client_get_status_code(s_client);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "request failed: %s%s", esp_err_to_name(err), attempt == 0 ? " (reconnecting)" : "");
        close_client();  // stale keep-alive or a network blip: retry once on a fresh connection
    }
    return ESP_FAIL;
}

// ---- tools -------------------------------------------------------------------------------------

std::string print(cJSON* j) {
    char* s = cJSON_PrintUnformatted(j);
    std::string out = s ? s : "";
    cJSON_free(s);
    cJSON_Delete(j);
    return out;
}

esp_err_t ensure_login(const Config& c) {
    if (va::logged_in()) return ESP_OK;
    std::string why;
    return va::login(c.va_user, c.va_password, &why);
}

std::string tool_search(const Config& c, cJSON* input, bool* is_error) {
    const cJSON* kw = cJSON_GetObjectItemCaseSensitive(input, "keyword");
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(input, "type");
    if (!cJSON_IsString(kw) || !kw->valuestring[0]) {
        *is_error = true;
        return "Missing required argument: keyword";
    }
    if (ensure_login(c) != ESP_OK) {
        *is_error = true;
        return "Could not sign in to the library.";
    }
    va::SearchResult r;
    if (va::search(kw->valuestring, r, 20, cJSON_IsString(type) ? type->valuestring : "Book") != ESP_OK) {
        *is_error = true;
        return "The library search failed.";
    }
    // Pre-cluster by author so the model summarises instead of reading a flat list (Bookworm's ResultSummarizer).
    std::vector<std::pair<std::string, std::vector<const va::BookHit*>>> groups;
    for (const auto& h : r.items) {
        std::string who = h.authors.empty() ? "(unknown author)" : h.authors;
        auto it = groups.begin();
        for (; it != groups.end(); ++it) if (it->first == who) break;
        if (it == groups.end()) {
            groups.push_back({who, {}});
            it = groups.end() - 1;
        }
        it->second.push_back(&h);
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "total", r.total);
    cJSON_AddNumberToObject(o, "shown", static_cast<double>(r.items.size()));
    cJSON_AddBoolToObject(o, "moreResultsExist", static_cast<int>(r.items.size()) < r.total);
    cJSON* by = cJSON_AddArrayToObject(o, "byAuthor");
    for (const auto& g : groups) {
        cJSON* go = cJSON_CreateObject();
        cJSON_AddStringToObject(go, "author", g.first.c_str());
        cJSON_AddNumberToObject(go, "count", static_cast<double>(g.second.size()));
        cJSON* titles = cJSON_AddArrayToObject(go, "titles");
        for (const va::BookHit* h : g.second) {
            cJSON* t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "title", h->title.c_str());
            cJSON_AddStringToObject(t, "bookshareId", h->bookshare_id.c_str());
            cJSON* fm = cJSON_AddArrayToObject(t, "formats");
            for (const auto& f : h->formats) cJSON_AddItemToArray(fm, cJSON_CreateString(f.c_str()));
            cJSON_AddItemToArray(titles, t);
        }
        cJSON_AddItemToArray(by, go);
    }
    return print(o);
}

void add_item(cJSON* arr, const va::ShelfItem& s) {
    cJSON* t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "title", s.title.c_str());
    cJSON_AddStringToObject(t, "author", s.author.c_str());
    cJSON_AddStringToObject(t, "bookshareId", s.bookshare_id.c_str());
    cJSON_AddStringToObject(t, "activeTitleId", s.active_title_id.c_str());
    cJSON_AddStringToObject(t, "status", s.status.c_str());
    cJSON_AddItemToArray(arr, t);
}

std::string tool_bookshelf(const Config& c, bool* is_error) {
    if (ensure_login(c) != ESP_OK) {
        *is_error = true;
        return "Could not sign in to the library.";
    }
    va::Shelf shelf;
    if (va::bookshelf(shelf) != ESP_OK) {
        *is_error = true;
        return "Could not read the bookshelf.";
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "loanCount", shelf.loan_count);
    cJSON_AddNumberToObject(o, "loanCap", va::kLoanCap);
    cJSON_AddNumberToObject(o, "freeSlots", va::kLoanCap - shelf.loan_count);
    cJSON_AddNumberToObject(o, "periodicalsOnShelf", shelf.periodical_count);
    cJSON* books = cJSON_AddArrayToObject(o, "books");
    for (const auto& b : shelf.books) add_item(books, b);
    return print(o);
}

std::string tool_request_list(const Config& c, bool* is_error) {
    if (ensure_login(c) != ESP_OK) {
        *is_error = true;
        return "Could not sign in to the library.";
    }
    std::vector<va::ShelfItem> items;
    int total = 0;
    if (va::request_list(items, &total) != ESP_OK) {
        *is_error = true;
        return "Could not read the request list.";
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "total", total);
    cJSON* arr = cJSON_AddArrayToObject(o, "items");
    for (const auto& s : items) add_item(arr, s);
    return print(o);
}

std::string arg(cJSON* input, const char* key) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(input, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

// Result for practice mode: nothing was sent to the library.
std::string dry_run_result(const char* action, const std::string& title) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "dryRun", true);
    cJSON_AddStringToObject(o, "action", action);
    cJSON_AddStringToObject(o, "title", title.c_str());
    cJSON_AddStringToObject(o, "note", "Practice mode: nothing was changed on the real library account.");
    return print(o);
}

std::string tool_add_bookshelf(const Config& c, cJSON* input, bool* is_error) {
    std::string id = arg(input, "bookshareId"), format = arg(input, "format"), title = arg(input, "title");
    std::string type = arg(input, "type");
    if (type.empty()) type = "book";
    if (id.empty() || format.empty()) {
        *is_error = true;
        return "Missing required argument: bookshareId and format are both needed.";
    }
    if (ensure_login(c) != ESP_OK) {
        *is_error = true;
        return "Could not sign in to the library.";
    }
    va::Shelf before;
    if (va::bookshelf(before) != ESP_OK) {
        *is_error = true;
        return "Could not read the bookshelf.";
    }
    // Check the 20-slot cap ourselves rather than letting the portal reject it silently.
    if (type != "periodical" && before.loan_count >= va::kLoanCap) {
        *is_error = true;
        return "The bookshelf is full (20 of 20 loan slots used). Offer to add it to the request list instead.";
    }
    for (const auto& b : before.books) {
        if (b.bookshare_id == id) {
            *is_error = true;
            return "That title is already on the bookshelf.";
        }
    }
    if (c.dry_run) return dry_run_result("add_to_bookshelf", title);

    std::string reply;
    if (va::add_to_bookshelf(id, format, type, &reply) != ESP_OK) {
        *is_error = true;
        return "The library did not accept that request.";
    }
    // Confirm by re-reading the shelf instead of trusting the response.
    va::Shelf after;
    bool present = false;
    if (va::bookshelf(after) == ESP_OK) {
        for (const auto& b : after.books) if (b.bookshare_id == id) present = true;
    }
    if (!present) {
        ESP_LOGW(TAG, "add: title not found on shelf afterwards (portal said: %s)", reply.c_str());
        *is_error = true;
        return "The library accepted the request but the title did not appear on the bookshelf. Tell the member it may not have worked.";
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", true);
    cJSON_AddStringToObject(o, "title", title.c_str());
    cJSON_AddNumberToObject(o, "freeSlotsNow", va::kLoanCap - after.loan_count);
    return print(o);
}

std::string tool_remove_bookshelf(const Config& c, cJSON* input, bool* is_error) {
    std::string active = arg(input, "activeTitleId");
    std::string type = arg(input, "type");
    if (type.empty()) type = "book";
    if (type != "book") {
        *is_error = true;
        return "Only books can be removed for now, not music or periodical issues.";
    }
    if (active.empty()) {
        *is_error = true;
        return "Missing required argument: activeTitleId";
    }
    if (ensure_login(c) != ESP_OK) {
        *is_error = true;
        return "Could not sign in to the library.";
    }
    va::Shelf before;
    if (va::bookshelf(before) != ESP_OK) {
        *is_error = true;
        return "Could not read the bookshelf.";
    }
    // Only remove something that is really on the shelf right now.
    std::string title;
    bool found = false;
    for (const auto& b : before.books) {
        if (b.active_title_id == active) {
            found = true;
            title = b.title;
        }
    }
    if (!found) {
        *is_error = true;
        return "That activeTitleId is not on the bookshelf. Call get_bookshelf and use an id from it.";
    }
    if (c.dry_run) return dry_run_result("remove_from_bookshelf", title);

    std::string reply;
    if (va::remove_from_bookshelf(active, type, &reply) != ESP_OK) {
        *is_error = true;
        return "The library did not accept that request.";
    }
    va::Shelf after;
    bool still_there = false;
    if (va::bookshelf(after) == ESP_OK) {
        for (const auto& b : after.books) if (b.active_title_id == active) still_there = true;
    }
    if (still_there) {
        ESP_LOGW(TAG, "remove: title still on shelf afterwards (portal said: %s)", reply.c_str());
        *is_error = true;
        return "The library accepted the request but the title is still on the bookshelf. Tell the member the removal did not work.";
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", true);
    cJSON_AddStringToObject(o, "title", title.c_str());
    cJSON_AddNumberToObject(o, "freeSlotsNow", va::kLoanCap - after.loan_count);
    return print(o);
}

std::string tool_add_request_list(const Config& c, cJSON* input, bool* is_error) {
    std::string id = arg(input, "bookshareId");
    if (id.empty()) {
        *is_error = true;
        return "Missing required argument: bookshareId";
    }
    if (ensure_login(c) != ESP_OK) {
        *is_error = true;
        return "Could not sign in to the library.";
    }
    if (c.dry_run) return dry_run_result("add_to_request_list", id);
    std::string reply;
    if (va::add_to_request_list(id, &reply) != ESP_OK) {
        *is_error = true;
        return "The library did not accept that request.";
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", true);
    cJSON_AddStringToObject(o, "bookshareId", id.c_str());
    return print(o);
}

std::string run_tool(const Config& c, const std::string& name, cJSON* input, bool* is_error) {
    if (name == "add_to_bookshelf") return tool_add_bookshelf(c, input, is_error);
    if (name == "remove_from_bookshelf") return tool_remove_bookshelf(c, input, is_error);
    if (name == "add_to_request_list") return tool_add_request_list(c, input, is_error);
    if (name == "search_library") return tool_search(c, input, is_error);
    if (name == "get_bookshelf") return tool_bookshelf(c, is_error);
    if (name == "get_request_list") return tool_request_list(c, is_error);
    *is_error = true;
    return "Unknown tool: " + name;
}

// ---- history -----------------------------------------------------------------------------------

void init_once() {
    if (!s_messages) s_messages = cJSON_CreateArray();
    if (!s_tools) s_tools = cJSON_Parse(kToolsJson);
}

void rollback_to(int size) {
    while (cJSON_GetArraySize(s_messages) > size) cJSON_DeleteItemFromArray(s_messages, cJSON_GetArraySize(s_messages) - 1);
}

// True for a message that starts a turn: a user message whose content is plain text, not tool results.
bool starts_turn(const cJSON* msg) {
    const cJSON* role = cJSON_GetObjectItemCaseSensitive(msg, "role");
    const cJSON* content = cJSON_GetObjectItemCaseSensitive(msg, "content");
    return cJSON_IsString(role) && strcmp(role->valuestring, "user") == 0 && cJSON_IsString(content);
}

// Drop the oldest whole turns so a tool_use is never separated from its tool_result.
void trim_history() {
    while (cJSON_GetArraySize(s_messages) > kMaxMessages) {
        int next = 1;
        int n = cJSON_GetArraySize(s_messages);
        while (next < n && !starts_turn(cJSON_GetArrayItem(s_messages, next))) next++;
        if (next >= n) break;
        for (int i = 0; i < next; i++) cJSON_DeleteItemFromArray(s_messages, 0);
    }
}

// Markdown-ish characters would be read out by the voice; drop them.
std::string speakable(std::string s) {
    std::string out;
    for (char ch : s) if (ch != '*' && ch != '#' && ch != '`') out += ch;
    return out;
}

}  // namespace

namespace brain {

void reset() {
    Lock lock;
    if (s_messages) rollback_to(0);
}

esp_err_t respond(const Config& c, const std::string& user_text, std::string& reply) {
    Lock lock;
    static const char kSorry[] = "Sorry, I ran into a problem. Could you try that again?";
    reply = kSorry;
    if (c.anthropic_key.empty()) {
        reply = "I need an Anthropic key before I can answer. Please add one on the setup page.";
        return ESP_ERR_INVALID_STATE;
    }
    init_once();
    if (!s_messages || !s_tools) return ESP_ERR_NO_MEM;

    int64_t now = esp_timer_get_time();
    if (s_last_turn_us && now - s_last_turn_us > kIdleResetUs) rollback_to(0);  // stale conversation

    const int checkpoint = cJSON_GetArraySize(s_messages);
    cJSON* user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", user_text.c_str());
    cJSON_AddItemToArray(s_messages, user);

    for (int round = 0; round < kMaxToolRoundTrips; round++) {
        JsonPtr req(cJSON_CreateObject());
        cJSON_AddStringToObject(req.get(), "model", CONFIG_BOOKBOOK_CLAUDE_MODEL);
        cJSON_AddNumberToObject(req.get(), "max_tokens", 1024);
        cJSON_AddStringToObject(req.get(), "system", kSystemPrompt);
        cJSON_AddItemReferenceToObject(req.get(), "messages", s_messages);  // referenced, not owned
        cJSON_AddItemReferenceToObject(req.get(), "tools", s_tools);
        char* raw = cJSON_PrintUnformatted(req.get());
        std::string body = raw ? raw : "";
        cJSON_free(raw);

        int64_t t0 = esp_timer_get_time();
        Response resp;
        esp_err_t err = call_claude(c, body, resp);
        ESP_LOGI(TAG, "round %d: HTTP %d, %u bytes out, %u in, %d ms", round + 1, resp.status,
                 static_cast<unsigned>(body.size()), static_cast<unsigned>(resp.body.size()),
                 static_cast<int>((esp_timer_get_time() - t0) / 1000));
        if (err != ESP_OK || resp.status != 200) {
            ESP_LOGE(TAG, "Claude call failed (%s, HTTP %d): %.300s", esp_err_to_name(err), resp.status, resp.body.c_str());
            rollback_to(checkpoint);
            if (resp.status == 401) reply = "My Anthropic key was rejected. Please check it on the setup page.";
            return ESP_FAIL;
        }
        JsonPtr parsed(cJSON_ParseWithLength(resp.body.data(), resp.body.size()));
        cJSON* content = parsed ? cJSON_GetObjectItemCaseSensitive(parsed.get(), "content") : nullptr;
        if (!cJSON_IsArray(content)) {
            rollback_to(checkpoint);
            return ESP_ERR_INVALID_RESPONSE;
        }

        // Keep the assistant turn exactly as returned (content blocks must round-trip verbatim).
        cJSON* assistant = cJSON_CreateObject();
        cJSON_AddStringToObject(assistant, "role", "assistant");
        cJSON_AddItemToObject(assistant, "content", cJSON_Duplicate(content, true));
        cJSON_AddItemToArray(s_messages, assistant);

        std::string text;
        cJSON* results = cJSON_CreateArray();
        cJSON* block;
        cJSON_ArrayForEach(block, content) {
            const cJSON* type = cJSON_GetObjectItemCaseSensitive(block, "type");
            if (!cJSON_IsString(type)) continue;
            if (strcmp(type->valuestring, "text") == 0) {
                const cJSON* t = cJSON_GetObjectItemCaseSensitive(block, "text");
                if (cJSON_IsString(t)) text += t->valuestring;
            } else if (strcmp(type->valuestring, "tool_use") == 0) {
                const cJSON* id = cJSON_GetObjectItemCaseSensitive(block, "id");
                const cJSON* name = cJSON_GetObjectItemCaseSensitive(block, "name");
                cJSON* input = cJSON_GetObjectItemCaseSensitive(block, "input");
                bool is_error = false;
                std::string nm = cJSON_IsString(name) ? name->valuestring : "";
                ESP_LOGI(TAG, "tool: %s", nm.c_str());
                std::string result = run_tool(c, nm, input, &is_error);
                cJSON* tr = cJSON_CreateObject();
                cJSON_AddStringToObject(tr, "type", "tool_result");
                cJSON_AddStringToObject(tr, "tool_use_id", cJSON_IsString(id) ? id->valuestring : "");
                cJSON_AddStringToObject(tr, "content", result.c_str());
                if (is_error) cJSON_AddBoolToObject(tr, "is_error", true);
                cJSON_AddItemToArray(results, tr);
            }
        }

        if (cJSON_GetArraySize(results) == 0) {
            cJSON_Delete(results);
            s_last_turn_us = esp_timer_get_time();
            trim_history();
            reply = speakable(text);
            if (reply.empty()) reply = "I am not sure what to say to that.";
            return ESP_OK;
        }
        cJSON* tool_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_msg, "role", "user");
        cJSON_AddItemToObject(tool_msg, "content", results);
        cJSON_AddItemToArray(s_messages, tool_msg);
    }

    rollback_to(checkpoint);
    reply = "Sorry, that took more steps than I can manage in one go. Could you ask again, maybe more specifically?";
    return ESP_FAIL;
}

}  // namespace brain
