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
#include "memory.h"
#include "va.h"

static const char* TAG = "brain";

namespace {

constexpr int kMaxToolRoundTrips = 8;  // then one last round, without tools, to say what was done
constexpr int kMaxMessages = 40;                       // history cap (trimmed at turn boundaries)
constexpr int64_t kIdleResetUs = 10LL * 60 * 1000000;  // forget the conversation after 10 idle minutes
constexpr size_t kMaxResponseBytes = 96 * 1024;

// The librarian's conversation rules, tuned for speech. Sections are marked "// == Title ==".
// docs/conversation-rules.md is generated from this block: run tools/rules_doc.py after editing.
const char kSystemPrompt[] =
    "You are BookBook, a voice librarian for a vision-impaired member of the Vision Australia Library. "
    "Everything you say is spoken aloud by a text-to-speech voice, and the member talks to you by holding a "
    "button, so what you receive is speech recognition and may be slightly wrong. There is no "
    "screen.\n\nRules:\n"
    // == Speaking ==
    "1. Keep replies short and natural, usually one to three sentences. No lists, bullet points, markdown, "
    "symbols, or anything that only makes sense visually.\n"
    "2. Never read out a long list. When a search or the bookshelf returns many items, group them by author "
    "or series and summarise in a sentence or two, then offer a next step. Reading out a shelf of up to about "
    "five books by title and author is fine.\n"
    "3. Ask at most one clarifying question at a time.\n"
    "4. Say author names in natural order, for example Tom Clancy. Never say ids, and never mention tool "
    "names.\n"
    "5. Answer only what was asked. Do not volunteer counts, free space, other shelves or summaries, and "
    "never read out status values such as READY_FOR_DOWNLOAD; a title that is on the shelf is simply there. "
    "An occasional short, friendly remark is welcome (for example on a good choice), but only now and then "
    "and never at the cost of the answer.\n"
    // == Who you are and what you do ==
    "6. Stay in scope: you help with the member's library, books, reading and lists. For anything else, such "
    "as the weather, news or general questions, say in one short sentence that you can only help with their "
    "books and library, and do not call any tools for it.\n"
    "7. Your name is Marian Paroo, named after the librarian in the musical 'The Music Man'. Only say so if "
    "you are asked your name or who you are; do not introduce yourself otherwise.\n"
    "8. If asked about your lights, explain them in the first person, in your own words, along these lines: "
    "'My coloured lights show my status. Solid green means I'm ready to answer your questions: just touch and "
    "hold my black grill and talk to me. Solid blue means I'm listening to your question, and spinning blue "
    "means I'm off getting answers or acting on your request. Red means I'm speaking.' Spinning yellow only "
    "appears while you are starting up and getting online.\n"
    "9. You can search the catalogue, list the bookshelf and the request list, add a title to the bookshelf "
    "or the request list, remove a book or a single periodical issue from the bookshelf, manage subscriptions "
    "to newspapers, magazines and podcasts, keep the member's lists (authors, books, On Hold), and remember "
    "things between sessions. You cannot yet remove from the request list. If asked for that, say so in one "
    "short sentence.\n"
    // == Searching the catalogue ==
    "10. The library catalogue only searches by title, author or series, never by subject or theme. For "
    "\"something like X\" requests, first think of specific titles or authors from your own knowledge, then "
    "use search_library to check what is actually available.\n"
    "11. search_library results include moreResultsExist. When it is true, do not imply you have heard the "
    "full set; say there are more and offer to narrow by author, series or era.\n"
    "12. Speech recognition may mishear names, for example Cornwall for Cornwell. If a name looks garbled, "
    "try the most likely intended author or title.\n"
    // == Bookshelf and request list ==
    "13. Adding to the bookshelf or the request list needs no confirmation: just do it, then say it is done "
    "in one short sentence. Removing from the bookshelf (a book or a periodical issue) is destructive: first "
    "say which title you would remove and ask whether to go ahead, and only call the remove tool after the "
    "member clearly says yes in their next message.\n"
    "14. When adding to the bookshelf, use a format from that title's formats list, preferring "
    "DAISY_Audio_Human, otherwise another audio format. If the bookshelf is full (no free slots), suggest "
    "putting the title On Hold, or alternatively adding it to the library's request list (which queues it "
    "with the library).\n"
    // == Subscriptions and periodical issues ==
    "15. Subscriptions are newspapers, magazines and podcasts whose new issues arrive on the bookshelf by "
    "themselves. To find one, search_library with type Newspaper, Magazine or Podcast, then "
    "subscribe_to_periodical straight away with that result's seriesId and format (prefer an audio format; if "
    "several results share a title, choose the audio one or ask). Unsubscribing is destructive: say which one "
    "you would cancel, wait for a yes, then call unsubscribe_from_periodical with the seriesId from "
    "get_subscriptions. Keep no other records of subscriptions: get_subscriptions is the list.\n"
    "16. Keep two things apart in what you say. A SUBSCRIPTION is a standing order: each new issue arrives on "
    "the bookshelf automatically (get_subscriptions). An ISSUE on the bookshelf is one copy of a newspaper or "
    "magazine that was added by hand (periodicalIssues in get_bookshelf); it is not a subscription and "
    "nothing replaces it when it is removed. Say 'subscribed to' only for subscriptions and 'an issue of' for "
    "shelf items. If asked which periodicals the member has, cover both in one answer, for example 'you are "
    "not subscribed to anything, but you have two issues on your bookshelf: X and Y'. When an issue on the "
    "shelf has no subscription behind it, offer once to subscribe them to it. When asked what is on the "
    "bookshelf, read the books first, then mention any issues. Removing an issue needs the same confirmation "
    "as removing a book.\n"
    // == Memory: preferences, authors and books ==
    "17. Use remember_preference whenever the member states a preference outside a normal search (favourite "
    "genres or authors, formats, things to avoid), and recall_preferences when it would help answer. What you "
    "already remember is listed below the rules.\n"
    "18. The member's authors list holds authors they know, and some of them are favourites. An author gets "
    "on the list when the member asks you to add them (add_preferred_author), and automatically when a book "
    "by them is added to the bookshelf, so never ask whether to add an author after adding a book. An author "
    "is a favourite only when the member says so: call add_preferred_author with isFavorite true (it also "
    "updates an author already on the list). After a successful add_to_bookshelf, use your own knowledge to "
    "name the title's likely genre; if it is not in currentPreferredGenres, ask whether to add it, and call "
    "add_preferred_genre only if they agree.\n"
    "19. The member's books list holds books they know, and some are favourites. A book gets on the list when "
    "the member asks you to add it (add_book), and automatically when it is added to the bookshelf. A book is "
    "a favourite only when the member says they like or love it: call add_book with isFavorite true. It works "
    "for any book, even one that was never on the bookshelf, and also updates a book already on the list. Use "
    "isFavorite false to take a book off the favourites but keep it on the list. The favourites are listed "
    "below as favoriteBooks.\n"
    "20. Removing an author from the member's preferred authors is destructive, so verify first: say which "
    "author you would remove and ask whether to go ahead, and only call remove_preferred_author after the "
    "member clearly says yes in their next message.\n"
    "21. Removing a book from the books list is destructive, so verify first: say which book you would remove "
    "and ask whether to go ahead, and only call remove_book after the member clearly says yes in their next "
    "message. If they only want it off the favourites, that needs no check: use add_book with isFavorite "
    "false.\n"
    "22. Use search_reading_history to check whether a title was read or borrowed before (it covers every "
    "book ever added, not just what is on the shelf), and rate_book whenever the member wants to rate a "
    "book.\n"
    "23. Ground 'what should I read next' and 'something like X' requests in the member's taste: call "
    "get_reading_profile for their frequent and favourite authors and favourite books. Never suggest, as "
    "something new, a book that is on their bookshelf or on their books list, since they already have it or "
    "know it; check your candidate titles against the profile and pick others. If they ask for a book they "
    "already know, that is fine.\n"
    "24. When the member asks what they have been reading lately or what kind of books they like, answer "
    "conversationally from the profile (favourite authors, a couple of favourites) rather than listing "
    "titles.\n"
    // == The On Hold list ==
    "25. There is an On Hold list: the member's own put-it-aside list, kept by you, separate from the "
    "library's request list. Books that will not fit on the bookshelf go On Hold. When the member has found a "
    "book or series they want but has not said what to do with it, offer the choice in one short question: "
    "put it on the bookshelf now, or put it On Hold. 'Add it' means the bookshelf; 'hold it', 'put it on "
    "hold', 'save it' or 'for later' means add_to_on_hold, which needs no confirmation. Always call it the On "
    "Hold list when you speak, and never say 'standby'. Every book is its own entry: when asked to put "
    "several books, or all the books in a series, On Hold, add each book separately (all in one "
    "add_to_on_hold call, using the books list, in reading order from your own knowledge), never as one entry "
    "with the titles in a note. A series is one entry only if the member explicitly asks for the series as "
    "one item.\n"
    "26. There are two ways to take a book off hold, and the member chooses: move it to the bookshelf (use "
    "add_to_bookshelf, finding its id and a format with search_library if the entry has no id; that takes it "
    "off hold by itself; if the entry is a series or is titled differently pass holdEntry, and pass "
    "keepOnHold only if the member wants it kept on hold too), or just delete it (remove_from_on_hold). "
    "Taking a book off hold does NOT mean deleting it: if the member only says something like 'take it off "
    "hold', 'release it' or 'un-hold it', do nothing yet and ask in one short question whether to put it on "
    "the bookshelf or just delete it. 'What books do I have on hold' is answered with get_on_hold_list.\n"
    "27. Deleting something from the On Hold list needs no confirmation: do it with remove_from_on_hold and "
    "say which book you removed. If several entries match, ask which one.\n"
    "28. Putting books On Hold does not need a catalogue search: do it straight away from your own knowledge, "
    "in a single add_to_on_hold call. Only search the catalogue when you are about to put a book on the "
    "bookshelf and need its id and format.\n"
    // == Practice mode ==
    "29. If a tool result says dryRun, the change was only pretended (practice mode). Tell the member it was "
    "a practice run and that nothing on their real library account changed.\n";

const char kToolsJson[] = R"JSON([
 {"name":"search_library",
  "description":"Search the Vision Australia Library catalogue by keyword. Only matches title, author or series title, never subject or synopsis. For thematic requests, propose specific candidate titles or authors yourself first, then call this to check availability.",
  "input_schema":{"type":"object","properties":{
    "keyword":{"type":"string","description":"Search keywords (title, author, or series)."},
    "type":{"type":"string","enum":["Book","Picture Book","Magazine","Newspaper","Podcast","Music"],"description":"Item type to search. Defaults to Book."}},
   "required":["keyword"]}},
 {"name":"get_bookshelf",
  "description":"Get the current bookshelf: the books on loan (including how many of the 20 loan slots are used) and, separately, any single newspaper or magazine issues on the shelf (periodicalIssues). Issues on the shelf are not subscriptions: use get_subscriptions for those.",
  "input_schema":{"type":"object","properties":{}}},
 {"name":"add_to_bookshelf",
  "description":"Add a title to the bookshelf (borrows it; uses one of the 20 loan slots). Do this straight away when asked, no confirmation needed. Use the bookshareId and a formatId from that title's search result.",
  "input_schema":{"type":"object","properties":{
    "bookshareId":{"type":"string","description":"The catalogue id from a search result."},
    "format":{"type":"string","description":"A formatId from the search result's formats list, e.g. DAISY_Audio_Human."},
    "title":{"type":"string","description":"The title, from the search result."},
    "author":{"type":"string","description":"The author, from the search result, if known."},
    "holdEntry":{"type":"string","description":"If this book is being moved off the On Hold list under a different title (for example a series), that entry's title. Otherwise omit."},
    "keepOnHold":{"type":"boolean","description":"True only if the member wants it to stay On Hold too. Normally omit: it is taken off hold when the book goes on the bookshelf."},
    "type":{"type":"string","enum":["book","music","periodical"],"description":"Defaults to book."}},
   "required":["bookshareId","format","title"]}},
 {"name":"remove_from_bookshelf",
  "description":"Remove a book, or a single periodical issue, from the bookshelf. Destructive: always say which title you would remove and get an explicit yes from the member in a prior message before calling this. For a periodical issue use type periodical and the activeTitleId from periodicalIssues in get_bookshelf; this does not cancel any subscription.",
  "input_schema":{"type":"object","properties":{
    "activeTitleId":{"type":"string","description":"The activeTitleId from get_bookshelf (not the bookshareId)."},
    "type":{"type":"string","enum":["book","music","periodical"],"description":"Defaults to book."}},
   "required":["activeTitleId"]}},
 {"name":"add_to_request_list",
  "description":"Add a title to the request list (no loan limit). Do this straight away when asked, no confirmation needed.",
  "input_schema":{"type":"object","properties":{
    "bookshareId":{"type":"string","description":"The catalogue id from a search result."}},
   "required":["bookshareId"]}},
 {"name":"get_subscriptions",
  "description":"Get the periodicals (newspapers, magazines, podcasts) the member is subscribed to. New issues arrive on the bookshelf by themselves.",
  "input_schema":{"type":"object","properties":{}}},
 {"name":"subscribe_to_periodical",
  "description":"Subscribe the member to a newspaper, magazine or podcast. Do this straight away when asked, no confirmation needed. Find it first with search_library (type Newspaper, Magazine or Podcast) and use that result's seriesId and one of its formats. If the same title appears more than once with different formats, pick the one that suits the member (they usually prefer audio) or ask.",
  "input_schema":{"type":"object","properties":{
    "seriesId":{"type":"string","description":"The seriesId from a periodical search result."},
    "format":{"type":"string","description":"A formatId from that search result's formats list."},
    "title":{"type":"string","description":"The periodical's title, from the search result."}},
   "required":["seriesId","format","title"]}},
 {"name":"unsubscribe_from_periodical",
  "description":"Cancel a subscription. Destructive: always say which periodical you would unsubscribe from and get an explicit yes from the member in a prior message before calling this. Use the seriesId from get_subscriptions.",
  "input_schema":{"type":"object","properties":{
    "seriesId":{"type":"string","description":"The seriesId from get_subscriptions."}},
   "required":["seriesId"]}},
 {"name":"remember_preference",
  "description":"Save a short note about something the member likes, dislikes or is looking for, so it is remembered in future sessions.",
  "input_schema":{"type":"object","properties":{
    "note":{"type":"string","description":"A short self-contained note, e.g. 'prefers DAISY Audio (Human)' or 'not keen on graphic violence'."}},
   "required":["note"]}},
 {"name":"recall_preferences",
  "description":"Get everything remembered about the member's stated preferences and ongoing interests.",
  "input_schema":{"type":"object","properties":{}}},
 {"name":"search_reading_history",
  "description":"Search the record of every title ever added to the bookshelf (including ones since removed), with dates and any rating. Use it to check whether a book was read before.",
  "input_schema":{"type":"object","properties":{
    "query":{"type":"string","description":"Title or author text to look for (partial match)."}},
   "required":["query"]}},
 {"name":"add_book",
  "description":"Put a book on the member's books list, and say whether it is a favourite. Call it when the member asks to add a book or says they like or love one. It works for any book, even one that was never on the bookshelf, and also updates a book already on the list; isFavorite false takes it off the favourites but keeps it on the list. (Books added to the bookshelf are put on the list automatically.)",
  "input_schema":{"type":"object","properties":{
    "title":{"type":"string","description":"The book's title."},
    "author":{"type":"string","description":"The author, if known, in natural order."},
    "isFavorite":{"type":"boolean","description":"Whether the member said it is a favourite (they like or love it)."},
    "bookshareId":{"type":"string","description":"The catalogue id from a search result, if you have it."}},
   "required":["title","isFavorite"]}},
 {"name":"remove_book",
  "description":"Remove a book from the member's books list entirely. Destructive: always say which book you would remove and get an explicit yes from the member in a prior message before calling this. To only take a book off the favourites, use add_book with isFavorite false instead.",
  "input_schema":{"type":"object","properties":{
    "title":{"type":"string","description":"The book's title (a partial title is fine if it matches only one book)."}},
   "required":["title"]}},
 {"name":"rate_book",
  "description":"Set a 1 to 5 star rating on a title in the reading history. Can be done at any time. Search the history first if unsure of the exact title.",
  "input_schema":{"type":"object","properties":{
    "title":{"type":"string","description":"The title to rate (partial match is fine if unambiguous)."},
    "rating":{"type":"integer","minimum":1,"maximum":5,"description":"1 to 5 stars."}},
   "required":["title","rating"]}},
 {"name":"get_preferred_authors",
  "description":"Get the authors the member has been asked about, including which are favourites.",
  "input_schema":{"type":"object","properties":{}}},
 {"name":"add_preferred_author",
  "description":"Put an author on the member's authors list, and say whether they are a favourite. Call it when the member asks to add an author or says an author is a favourite; it also updates an author already on the list. (Authors of books added to the bookshelf are added automatically.)",
  "input_schema":{"type":"object","properties":{
    "authorName":{"type":"string","description":"The author's name in natural order, e.g. Tom Clancy."},
    "isFavorite":{"type":"boolean","description":"Whether the member said this is a favourite author."}},
   "required":["authorName","isFavorite"]}},
 {"name":"get_preferred_genres",
  "description":"Get the genres the member has agreed to add as preferences.",
  "input_schema":{"type":"object","properties":{}}},
 {"name":"remove_preferred_author",
  "description":"Remove an author from the member's preferred authors. Destructive: always say which author you would remove and get an explicit yes from the member in a prior message before calling this.",
  "input_schema":{"type":"object","properties":{
    "authorName":{"type":"string","description":"The author's name, as listed in preferredAuthors (natural order, e.g. Tom Clancy)."}},
   "required":["authorName"]}},
 {"name":"add_preferred_genre",
  "description":"Record a genre as preferred. Only call this after actually asking the member.",
  "input_schema":{"type":"object","properties":{
    "genre":{"type":"string","description":"A short genre label, e.g. Historical Fiction."}},
   "required":["genre"]}},
 {"name":"get_reading_profile",
  "description":"Get a summary of the member's taste: their most frequent authors, favourite authors and books, what is on the bookshelf now, and their books list. Use it to ground 'what should I read next' and 'something like X' requests, to recognise books they already have or have read so they are not suggested again, and to answer 'what have I been reading lately?'.",
  "input_schema":{"type":"object","properties":{}}},
 {"name":"add_to_on_hold",
  "description":"Put books On Hold: the member's own put-it-aside list, for books they want but cannot fit on the bookshelf right now, or want to save for later. It is separate from the library's request list (which really queues a title with the library). Do this straight away when asked; no confirmation needed. EVERY BOOK IS ITS OWN ENTRY, so each can later be moved to the bookshelf or deleted on its own: if the member asks for several books, or all the books in a series, pass them all in `books`, one item per book, listing them in reading order from your own knowledge of the series. Do not squeeze a list of titles into one entry's note. Only save a series as a single entry if the member explicitly asks for the series as one item.",
  "input_schema":{"type":"object","properties":{
    "books":{"type":"array","description":"Several books in one call (preferred when there is more than one).","items":{"type":"object","properties":{
      "title":{"type":"string","description":"One book's title."},
      "author":{"type":"string","description":"The author, in natural order."},
      "bookshareId":{"type":"string","description":"The catalogue id from a search result, if you have it."},
      "note":{"type":"string","description":"A short note, e.g. 'James Bond #1' or 'series: The Last Kingdom, book 1'."}},
      "required":["title"]}},
    "title":{"type":"string","description":"For a single book: its title."},
    "author":{"type":"string","description":"For a single book: the author, in natural order."},
    "bookshareId":{"type":"string","description":"For a single book: the catalogue id, if you have it."},
    "note":{"type":"string","description":"For a single book: a short note."}}}},
 {"name":"get_on_hold_list",
  "description":"Get everything on the member's On Hold list.",
  "input_schema":{"type":"object","properties":{}}},
 {"name":"remove_from_on_hold",
  "description":"Delete an entry from the On Hold list without putting it on the bookshelf. Call it only when the member clearly says to delete, remove or forget the book (or answers 'just delete it' to your question); no further confirmation is needed. 'Take it off hold', 'release it' or 'un-hold it' do NOT say what to do with the book: ask first whether to put it on the bookshelf or just delete it. (When a book moves to the bookshelf, add_to_bookshelf takes it off hold by itself.)",
  "input_schema":{"type":"object","properties":{
    "title":{"type":"string","description":"The entry's title (a partial title is fine if it matches only one entry)."}},
   "required":["title"]}},
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
    cfg.timeout_ms = 25000;  // a stalled call must not hold the member up for a minute
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
    std::string why;
    return va::ensure_logged_in(c.va_user, c.va_password, &why);
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
    const std::string kind = cJSON_IsString(type) ? type->valuestring : "Book";
    if (kind == "Magazine" || kind == "Newspaper" || kind == "Podcast") {
        // Periodicals have no authors. The same title can appear once per format, each with its own seriesId.
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "total", r.total);
        cJSON_AddNumberToObject(o, "shown", static_cast<double>(r.items.size()));
        cJSON* arr = cJSON_AddArrayToObject(o, "periodicals");
        for (const auto& h : r.items) {
            cJSON* t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "title", h.title.c_str());
            cJSON_AddStringToObject(t, "seriesId", h.bookshare_id.c_str());
            cJSON* fm = cJSON_AddArrayToObject(t, "formats");
            for (const auto& f : h.formats) cJSON_AddItemToArray(fm, cJSON_CreateString(f.c_str()));
            cJSON_AddItemToArray(arr, t);
        }
        return print(o);
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
    // Single newspaper/magazine issues on the shelf. These are NOT subscriptions.
    cJSON* issues = cJSON_AddArrayToObject(o, "periodicalIssues");
    for (const auto& p : shelf.periodicals) {
        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "title", p.title.c_str());
        cJSON_AddStringToObject(t, "issueDate", p.issue_date.c_str());
        cJSON_AddStringToObject(t, "format", p.format.c_str());
        cJSON_AddStringToObject(t, "activeTitleId", p.active_title_id.c_str());
        cJSON_AddItemToArray(issues, t);
    }
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

std::string tool_profile(const Config& c, bool* is_error) {
    va::Shelf shelf;  // if the library cannot be reached the profile is built from the books list alone
    if (ensure_login(c) != ESP_OK || va::bookshelf(shelf) != ESP_OK) {
        ESP_LOGW(TAG, "profile: bookshelf unavailable, using the books list only");
        shelf = va::Shelf();
    }
    if (memory::configured(c)) memory::refresh(c);  // loads on first use, then only when changed elsewhere
    (void)is_error;
    return memory::reading_profile(shelf);
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
    std::string author = va::natural_author(arg(input, "author"));
    const bool keep_on_standby = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input, "keepOnHold"));
    const std::string standby_entry = arg(input, "holdEntry");
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
        return "The bookshelf is full (20 of 20 loan slots used). Suggest putting it On Hold instead, or adding it to the request list.";
    }
    for (const auto& b : before.books) {
        if (b.bookshare_id == id) {
            *is_error = true;
            return "That title is already on the bookshelf.";
        }
    }
    // Read-only, so it is safe in practice mode too: lets the ask-about-author/genre flow be rehearsed.
    memory::AddInfo info = memory::add_info(author);
    if (c.dry_run) {
        cJSON* o = cJSON_Parse(dry_run_result("add_to_bookshelf", title).c_str());
        cJSON_AddBoolToObject(o, "authorAlreadyInPreferredAuthors", info.author_known);
        cJSON_AddItemToObject(o, "currentPreferredGenres", cJSON_Parse(info.genres_json.c_str()));
        return print(o);
    }

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
    if (memory::configured(c)) {
        bool author_added = false;
        bool logged = memory::log_added(c, title, author, id, &author_added) == ESP_OK;
        cJSON_AddBoolToObject(o, "loggedToReadingHistory", logged);
        cJSON_AddBoolToObject(o, "authorAddedToAuthorsList", author_added);
        // Moving a book from the standby list to the bookshelf takes it off standby (unless asked to keep it).
        if (!keep_on_standby) {
            std::string taken;
            if (memory::standby_take(c, id, title, standby_entry, &taken) == ESP_OK && !taken.empty()) {
                cJSON_AddStringToObject(o, "removedFromOnHold", taken.c_str());
            }
        }
    }
    cJSON_AddBoolToObject(o, "authorAlreadyInPreferredAuthors", info.author_known);
    cJSON_AddItemToObject(o, "currentPreferredGenres", cJSON_Parse(info.genres_json.c_str()));
    return print(o);
}

std::string tool_remove_bookshelf(const Config& c, cJSON* input, bool* is_error) {
    std::string active = arg(input, "activeTitleId");
    std::string type = arg(input, "type");
    if (type.empty()) type = "book";
    if (type != "book" && type != "periodical") {
        *is_error = true;
        return "Only books and periodical issues can be removed for now, not music.";
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
    std::string title, bookshare_id;
    bool found = false;
    const std::vector<va::ShelfItem>& pool = type == "periodical" ? before.periodicals : before.books;
    for (const auto& b : pool) {
        if (b.active_title_id == active) {
            found = true;
            title = b.title;
            if (type == "periodical" && !b.issue_date.empty()) title += ", issue of " + b.issue_date;
            bookshare_id = b.bookshare_id;
        }
    }
    if (!found) {
        *is_error = true;
        return "That activeTitleId is not on the bookshelf. Call get_bookshelf and use an id from it.";
    }
    if (c.dry_run) return dry_run_result("remove_from_bookshelf", title);

    std::string reply;
    const bool issue = type == "periodical";
    if ((issue ? va::remove_periodical_issue(active, &reply) : va::remove_from_bookshelf(active, type, &reply)) != ESP_OK) {
        *is_error = true;
        return "The library did not accept that request.";
    }
    va::Shelf after;
    bool still_there = false;
    if (va::bookshelf(after) == ESP_OK) {
        for (const auto& b : issue ? after.periodicals : after.books) if (b.active_title_id == active) still_there = true;
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
    if (!issue && memory::configured(c)) memory::log_removed(c, bookshare_id);
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

std::string tool_get_subscriptions(const Config& c, bool* is_error) {
    if (ensure_login(c) != ESP_OK) {
        *is_error = true;
        return "Could not sign in to the library.";
    }
    std::vector<va::Subscription> subs;
    if (va::subscriptions(subs) != ESP_OK) {
        *is_error = true;
        return "Could not read the subscriptions.";
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "total", static_cast<double>(subs.size()));
    cJSON* arr = cJSON_AddArrayToObject(o, "subscriptions");
    for (const auto& s : subs) {
        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "title", s.title.c_str());
        cJSON_AddStringToObject(t, "seriesId", s.series_id.c_str());
        cJSON_AddStringToObject(t, "format", s.format.c_str());
        cJSON_AddStringToObject(t, "kind", s.kind.c_str());
        cJSON_AddItemToArray(arr, t);
    }
    return print(o);
}

bool has_subscription(const std::vector<va::Subscription>& subs, const std::string& id) {
    for (const auto& s : subs) if (s.series_id == id) return true;
    return false;
}

std::string tool_subscribe(const Config& c, cJSON* input, bool* is_error) {
    std::string id = arg(input, "seriesId"), format = arg(input, "format"), title = arg(input, "title");
    if (id.empty() || format.empty()) {
        *is_error = true;
        return "Missing required argument: seriesId and format are both needed.";
    }
    if (ensure_login(c) != ESP_OK) {
        *is_error = true;
        return "Could not sign in to the library.";
    }
    std::vector<va::Subscription> before;
    if (va::subscriptions(before) != ESP_OK) {
        *is_error = true;
        return "Could not read the subscriptions.";
    }
    if (has_subscription(before, id)) {
        *is_error = true;
        return "The member is already subscribed to that periodical.";
    }
    if (c.dry_run) return dry_run_result("subscribe_to_periodical", title);
    std::string reply;
    if (va::subscribe(id, format, title, &reply) != ESP_OK) {
        *is_error = true;
        return "The library did not accept that request.";
    }
    std::vector<va::Subscription> after;  // confirm by re-reading rather than trusting the response
    if (va::subscriptions(after) != ESP_OK || !has_subscription(after, id)) {
        ESP_LOGW(TAG, "subscribe: not in the list afterwards (portal said: %s)", reply.c_str());
        *is_error = true;
        return "The library accepted the request but the subscription did not appear. Tell the member it may not have worked.";
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", true);
    cJSON_AddStringToObject(o, "title", title.c_str());
    cJSON_AddStringToObject(o, "note", "New issues will arrive on the bookshelf by themselves.");
    return print(o);
}

std::string tool_unsubscribe(const Config& c, cJSON* input, bool* is_error) {
    std::string id = arg(input, "seriesId");
    if (id.empty()) {
        *is_error = true;
        return "Missing required argument: seriesId";
    }
    if (ensure_login(c) != ESP_OK) {
        *is_error = true;
        return "Could not sign in to the library.";
    }
    std::vector<va::Subscription> before;
    if (va::subscriptions(before) != ESP_OK) {
        *is_error = true;
        return "Could not read the subscriptions.";
    }
    std::string title;
    for (const auto& s : before) if (s.series_id == id) title = s.title;
    if (title.empty()) {
        *is_error = true;
        return "That seriesId is not in the subscriptions. Call get_subscriptions and use an id from it.";
    }
    if (c.dry_run) return dry_run_result("unsubscribe_from_periodical", title);
    std::string reply;
    if (va::unsubscribe(id, &reply) != ESP_OK) {
        *is_error = true;
        return "The library did not accept that request.";
    }
    std::vector<va::Subscription> after;
    if (va::subscriptions(after) != ESP_OK || has_subscription(after, id)) {
        ESP_LOGW(TAG, "unsubscribe: still in the list afterwards (portal said: %s)", reply.c_str());
        *is_error = true;
        return "The library accepted the request but the subscription is still there. Tell the member it did not work.";
    }
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", true);
    cJSON_AddStringToObject(o, "title", title.c_str());
    return print(o);
}

std::string memory_tool(const Config& c, const std::string& name, cJSON* input, bool* is_error) {
    if (!memory::configured(c)) {
        *is_error = true;
        return "Memory storage is not set up on this device yet, so nothing can be remembered.";
    }
    memory::refresh(c);  // pick up changes made on another device
    if (!memory::loaded() && memory::load(c) != ESP_OK) {
        *is_error = true;
        return "Could not reach the memory storage.";
    }
    auto ok_or_fail = [&](esp_err_t e, const char* what) -> std::string {
        if (e != ESP_OK) {
            *is_error = true;
            return std::string("Could not save ") + what + ".";
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "success", true);
        return print(o);
    };
    if (name == "remember_preference") {
        std::string note = arg(input, "note");
        if (note.empty()) {
            *is_error = true;
            return "Missing required argument: note";
        }
        return ok_or_fail(memory::remember(c, note), "the note");
    }
    if (name == "add_to_on_hold") {
        std::vector<memory::StandbyEntry> entries;
        const cJSON* many = cJSON_GetObjectItemCaseSensitive(input, "books");
        if (cJSON_IsArray(many)) {
            const cJSON* b;
            cJSON_ArrayForEach(b, many) {
                std::string t = arg(const_cast<cJSON*>(b), "title");
                if (!t.empty()) {
                    entries.push_back({t, arg(const_cast<cJSON*>(b), "author"), arg(const_cast<cJSON*>(b), "bookshareId"),
                                       arg(const_cast<cJSON*>(b), "note")});
                }
            }
        }
        if (entries.empty() && !arg(input, "title").empty()) {
            entries.push_back({arg(input, "title"), arg(input, "author"), arg(input, "bookshareId"), arg(input, "note")});
        }
        if (entries.empty()) {
            *is_error = true;
            return "Nothing to add: give a title, or a books list.";
        }
        int created = 0, updated = 0;
        if (memory::standby_add_many(c, entries, &created, &updated) != ESP_OK) {
            *is_error = true;
            return "Could not save to the On Hold list.";
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "success", true);
        cJSON_AddNumberToObject(o, "newOnHold", created);
        cJSON_AddNumberToObject(o, "alreadyOnHold", updated);
        return print(o);
    }
    if (name == "get_on_hold_list") {
        std::string json;
        if (memory::standby_list(c, &json) != ESP_OK) {
            *is_error = true;
            return "Could not read the On Hold list.";
        }
        return json;
    }
    if (name == "remove_from_on_hold") {
        std::string title = arg(input, "title");
        if (title.empty()) {
            *is_error = true;
            return "Missing required argument: title";
        }
        std::string matched;
        int matches = 0;
        esp_err_t e = memory::standby_remove(c, title, &matched, &matches);
        if (e == ESP_ERR_NOT_FOUND) {
            *is_error = true;
            return "Nothing on the On Hold list matches that. Call get_on_hold_list to see what is there.";
        }
        if (e == ESP_ERR_INVALID_SIZE) {
            *is_error = true;
            return std::to_string(matches) + " entries match that; ask the member which one.";
        }
        if (e != ESP_OK) {
            *is_error = true;
            return "Could not save that change.";
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "success", true);
        cJSON_AddStringToObject(o, "removed", matched.c_str());
        return print(o);
    }
    if (name == "recall_preferences") return memory::recall();
    if (name == "search_reading_history") return memory::search_history(arg(input, "query"));
    if (name == "get_preferred_authors") return memory::preferred_authors_json();
    if (name == "get_preferred_genres") return memory::preferred_genres_json();
    if (name == "add_preferred_author") {
        std::string who = arg(input, "authorName");
        const cJSON* fav = cJSON_GetObjectItemCaseSensitive(input, "isFavorite");
        if (who.empty() || !cJSON_IsBool(fav)) {
            *is_error = true;
            return "Missing required arguments: authorName and isFavorite";
        }
        return ok_or_fail(memory::add_author(c, who, cJSON_IsTrue(fav)), "the author");
    }
    if (name == "remove_preferred_author") {
        std::string who = arg(input, "authorName");
        if (who.empty()) {
            *is_error = true;
            return "Missing required argument: authorName";
        }
        std::string matched;
        int matches = 0;
        esp_err_t e = memory::remove_author(c, who, &matched, &matches);
        if (e == ESP_ERR_NOT_FOUND) {
            *is_error = true;
            return "That author is not in the preferred authors. Call get_preferred_authors to see who is.";
        }
        if (e == ESP_ERR_INVALID_SIZE) {
            *is_error = true;
            return std::to_string(matches) + " authors match that; ask the member which one.";
        }
        if (e != ESP_OK) {
            *is_error = true;
            return "Could not save that change.";
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "success", true);
        cJSON_AddStringToObject(o, "removedAuthor", matched.c_str());
        return print(o);
    }
    if (name == "add_preferred_genre") {
        std::string genre = arg(input, "genre");
        if (genre.empty()) {
            *is_error = true;
            return "Missing required argument: genre";
        }
        return ok_or_fail(memory::add_genre(c, genre), "the genre");
    }
    if (name == "add_book") {
        std::string title = arg(input, "title");
        const cJSON* fav = cJSON_GetObjectItemCaseSensitive(input, "isFavorite");
        if (title.empty() || !cJSON_IsBool(fav)) {
            *is_error = true;
            return "Missing required arguments: title and isFavorite";
        }
        bool created = false;
        if (memory::add_book(c, title, arg(input, "author"), arg(input, "bookshareId"), cJSON_IsTrue(fav), &created) != ESP_OK) {
            *is_error = true;
            return "Could not save that book.";
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "success", true);
        cJSON_AddStringToObject(o, "title", title.c_str());
        cJSON_AddBoolToObject(o, "isFavorite", cJSON_IsTrue(fav));
        cJSON_AddBoolToObject(o, "newOnList", created);
        return print(o);
    }
    if (name == "remove_book") {
        std::string title = arg(input, "title");
        if (title.empty()) {
            *is_error = true;
            return "Missing required argument: title";
        }
        std::string matched;
        int matches = 0;
        esp_err_t e = memory::remove_book(c, title, &matched, &matches);
        if (e == ESP_ERR_NOT_FOUND) {
            *is_error = true;
            return "No book on the list matches that title. Use search_reading_history to check.";
        }
        if (e == ESP_ERR_INVALID_SIZE) {
            *is_error = true;
            return std::to_string(matches) + " books match that; ask the member which one.";
        }
        if (e != ESP_OK) {
            *is_error = true;
            return "Could not save that change.";
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "success", true);
        cJSON_AddStringToObject(o, "removedBook", matched.c_str());
        return print(o);
    }
    if (name == "rate_book") {
        std::string title = arg(input, "title");
        const cJSON* rating = cJSON_GetObjectItemCaseSensitive(input, "rating");
        if (title.empty() || !cJSON_IsNumber(rating) || rating->valueint < 1 || rating->valueint > 5) {
            *is_error = true;
            return "rate_book needs a title and a rating from 1 to 5.";
        }
        std::string matched;
        int matches = 0;
        esp_err_t e = memory::rate(c, title, rating->valueint, &matched, &matches);
        if (e == ESP_ERR_NOT_FOUND) {
            *is_error = true;
            return "No title in the reading history matches that. Use search_reading_history to find it, or if the member simply likes this book, use add_book.";
        }
        if (e == ESP_ERR_INVALID_SIZE) {
            *is_error = true;
            return std::to_string(matches) + " titles match that; ask the member which one, or use a longer part of the title.";
        }
        if (e != ESP_OK) {
            *is_error = true;
            return "Could not save the rating.";
        }
        cJSON* o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "success", true);
        cJSON_AddStringToObject(o, "title", matched.c_str());
        cJSON_AddNumberToObject(o, "rating", rating->valueint);
        return print(o);
    }
    *is_error = true;
    return "Unknown tool: " + name;
}

std::string run_tool(const Config& c, const std::string& name, cJSON* input, bool* is_error) {
    if (name == "remember_preference" || name == "recall_preferences" || name == "search_reading_history" ||
        name == "rate_book" || name == "add_book" || name == "remove_book" || name == "add_to_on_hold" ||
        name == "get_on_hold_list" || name == "remove_from_on_hold" || name == "get_preferred_authors" || name == "add_preferred_author" ||
        name == "get_preferred_genres" || name == "add_preferred_genre" || name == "remove_preferred_author") {
        return memory_tool(c, name, input, is_error);
    }
    if (name == "get_reading_profile") return tool_profile(c, is_error);
    if (name == "add_to_bookshelf") return tool_add_bookshelf(c, input, is_error);
    if (name == "remove_from_bookshelf") return tool_remove_bookshelf(c, input, is_error);
    if (name == "add_to_request_list") return tool_add_request_list(c, input, is_error);
    if (name == "search_library") return tool_search(c, input, is_error);
    if (name == "get_bookshelf") return tool_bookshelf(c, is_error);
    if (name == "get_request_list") return tool_request_list(c, is_error);
    if (name == "get_subscriptions") return tool_get_subscriptions(c, is_error);
    if (name == "subscribe_to_periodical") return tool_subscribe(c, input, is_error);
    if (name == "unsubscribe_from_periodical") return tool_unsubscribe(c, input, is_error);
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

esp_err_t respond(const Config& c, const std::string& user_text, std::string& reply, const std::atomic<bool>* cancel) {
    Lock lock;
    static const char kSorry[] = "Sorry, I ran into a problem. Could you try that again?";
    reply = kSorry;
    if (c.anthropic_key.empty()) {
        reply = "I need an Anthropic key before I can answer. Please add one on the setup page.";
        return ESP_ERR_INVALID_STATE;
    }
    init_once();
    if (!s_messages || !s_tools) return ESP_ERR_NO_MEM;
    // Loaded at startup; a NEW conversation also checks whether another device changed anything since.
    if (memory::configured(c) && (!memory::loaded() || cJSON_GetArraySize(s_messages) == 0)) {
        memory::refresh(c);  // loads if not loaded yet (waits for the start-up warm-up), else checks for changes
    }

    int64_t now = esp_timer_get_time();
    if (s_last_turn_us && now - s_last_turn_us > kIdleResetUs) rollback_to(0);  // stale conversation

    const int checkpoint = cJSON_GetArraySize(s_messages);
    cJSON* user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "role", "user");
    cJSON_AddStringToObject(user, "content", user_text.c_str());
    cJSON_AddItemToArray(s_messages, user);

    auto cancelled = [&] { return cancel && cancel->load(); };
    for (int round = 0; round <= kMaxToolRoundTrips; round++) {
        if (cancelled()) {
            ESP_LOGI(TAG, "turn abandoned before round %d", round + 1);
            rollback_to(checkpoint);
            reply.clear();
            return ESP_ERR_INVALID_STATE;
        }
        // After kMaxToolRoundTrips rounds of tool calls, one more round WITHOUT tools: the changes already made
        // are real, so the member must be told what was done (this used to discard the work and report failure).
        const bool last_round = round == kMaxToolRoundTrips;
        JsonPtr req(cJSON_CreateObject());
        cJSON_AddStringToObject(req.get(), "model", CONFIG_BOOKBOOK_CLAUDE_MODEL);
        cJSON_AddNumberToObject(req.get(), "max_tokens", 1024);
        std::string system = std::string(kSystemPrompt) + "\nRemembered from previous sessions:\n" + memory::prompt_snapshot();
        if (last_round) {
            system += "\nYou have used every step you are allowed this turn. Do not call any more tools. Tell the member "
                      "briefly, in one or two sentences, what you have already done and what is still left, and offer to "
                      "continue if they ask again.\n";
            cJSON* none = cJSON_AddObjectToObject(req.get(), "tool_choice");
            cJSON_AddStringToObject(none, "type", "none");
        }
        cJSON_AddStringToObject(req.get(), "system", system.c_str());
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
            if (cancelled()) {
                rollback_to(checkpoint);
                reply.clear();
                return ESP_ERR_INVALID_STATE;
            }
            if (last_round) {  // tools already ran: do not throw the work away and do not claim it failed
                reply = "I got part of the way through that but ran out of steps. Please ask me to check your lists.";
                s_last_turn_us = esp_timer_get_time();
                return ESP_FAIL;
            }
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

        if (last_round) {  // text only, whatever the model sent: a stray tool_use with no result would corrupt the history
            std::string said;
            const cJSON* b;
            cJSON_ArrayForEach(b, content) {
                const cJSON* ty = cJSON_GetObjectItemCaseSensitive(b, "type");
                const cJSON* tx = cJSON_GetObjectItemCaseSensitive(b, "text");
                if (cJSON_IsString(ty) && strcmp(ty->valuestring, "text") == 0 && cJSON_IsString(tx)) said += tx->valuestring;
            }
            cJSON* am = cJSON_CreateObject();
            cJSON_AddStringToObject(am, "role", "assistant");
            cJSON_AddStringToObject(am, "content", said.c_str());
            cJSON_AddItemToArray(s_messages, am);
            s_last_turn_us = esp_timer_get_time();
            trim_history();
            reply = speakable(said);
            if (reply.empty()) reply = "I did part of that but ran out of steps. Please ask me to check your lists.";
            return ESP_OK;
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
                if (cancelled()) {  // the member gave up: do not start another step
                    cJSON_Delete(results);
                    rollback_to(checkpoint);
                    reply.clear();
                    return ESP_ERR_INVALID_STATE;
                }
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

    rollback_to(checkpoint);  // not reached: the last round always returns
    reply = "Sorry, that took more steps than I can manage in one go. Could you ask again, maybe more specifically?";
    return ESP_FAIL;
}

}  // namespace brain
