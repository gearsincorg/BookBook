#pragma once
#include <string>
#include <vector>
#include "esp_err.h"

// Vision Australia Library client. Mirrors Bookworm.Core's VaLibraryClient (see Bookworm's
// docs/va-endpoints.md): an unofficial, session-cookie JSON API behind my.visionaustralia.org.
// Behaves like one polite human user: custom User-Agent, >= 300 ms between calls, one connection.
// Read-only for now (login, search, bookshelf, request list). All calls are thread-safe.
namespace va {

constexpr int kLoanCap = 20;  // books + music + braille on the bookshelf

struct BookHit {
    std::string bookshare_id;  // catalogue id: use for add-to-bookshelf / request-list
    std::vector<std::string> formats;  // formatIds, e.g. DAISY_Audio_Human
    std::string title;
    std::string authors;
    std::string status;  // e.g. READY_FOR_DOWNLOAD
};

struct SearchResult {
    int total = 0;  // matches in the catalogue; items may hold fewer
    std::vector<BookHit> items;
};

struct ShelfItem {
    std::string bookshare_id;
    std::string active_title_id;  // loan-instance id: use for remove
    std::string title;
    std::string author;
    std::string format;
    std::string status;
    std::string date_added;
    std::string issue_date;  // periodical issues only: the publication date, e.g. "17 September 2026"
};

struct Shelf {
    int loan_count = 0;  // books + music + braille, out of kLoanCap
    int periodical_count = 0;
    std::vector<ShelfItem> books;
    // Single issues of newspapers and magazines, borrowed one at a time (they are not loan slots). For these,
    // active_title_id holds the id that remove_periodical_issue() needs (the issue's own id).
    std::vector<ShelfItem> periodicals;
};

// A periodical (magazine, newspaper, podcast) the member is subscribed to. New issues arrive on the
// bookshelf by themselves.
struct Subscription {
    std::string series_id;  // the periodical's id: use for unsubscribe (same as bookshareId in a search)
    std::string title;
    std::string format;  // display name, e.g. "DAISY Text"
    std::string kind;    // Newspaper, Magazine, Podcast
};

// Catalogue names look like "Silva, Daniel, 1960-" or "By Smith, Martin Cruz, 1942-". This returns the
// natural spoken form ("Daniel Silva", "Martin Cruz Smith"): dates dropped, first name first. Several
// authors separated by ';' come back joined with " and ".
std::string natural_author(const std::string& catalogue_name);

// Sets the credentials and logs in (3-step handshake). On failure `error` is a short reason.
esp_err_t login(const std::string& user, const std::string& password, std::string* error = nullptr);
bool logged_in();
// Logs in unless this user is already logged in. Waits for a login already in progress (for example the
// start-up warm-up) instead of starting a second one.
esp_err_t ensure_logged_in(const std::string& user, const std::string& password, std::string* error = nullptr);

// `type` is the portal's own dropdown value: "Book", "Picture Book", "Magazine", ...
esp_err_t search(const std::string& keyword, SearchResult& out, int limit = 20, const char* type = "Book");
esp_err_t bookshelf(Shelf& out);

// Write operations (they change the real account). `type` is "book", "music" or "periodical" (anything
// else is treated as "book"). `reply` receives the portal's short answer for logging. Adding is verified
// live. Removal only supports books (POST .../remove/all, as the site's Remove Selected button does).
// Callers should still re-read the shelf afterwards to confirm the change really happened.
esp_err_t add_to_bookshelf(const std::string& bookshare_id, const std::string& format, const std::string& type,
                           std::string* reply = nullptr);
esp_err_t remove_from_bookshelf(const std::string& active_title_id, const std::string& type,
                                std::string* reply = nullptr);
// Removes ONE periodical issue from the bookshelf: POST /library/my-periodical/remove/all with
// periodical_active_title_ids=<the issue's id>, as the site's Remove Selected button does (verified live, 2026-09).
esp_err_t remove_periodical_issue(const std::string& issue_id, std::string* reply = nullptr);
esp_err_t add_to_request_list(const std::string& bookshare_id, std::string* reply = nullptr);
esp_err_t request_list(std::vector<ShelfItem>& out, int* total = nullptr);

// Subscriptions (verified live, 2026-09): list, subscribe (POST .../subscription/add/?seriesId=..&format=..)
// and unsubscribe (DELETE .../subscription/remove/{seriesId}). Search for periodicals with
// search(keyword, out, n, "Magazine" | "Newspaper" | "Podcast"): each hit's bookshare_id is the series id and
// its formats hold the one formatId that hit is available in.
esp_err_t subscriptions(std::vector<Subscription>& out);
esp_err_t subscribe(const std::string& series_id, const std::string& format, const std::string& title,
                    std::string* reply = nullptr);
esp_err_t unsubscribe(const std::string& series_id, std::string* reply = nullptr);

}  // namespace va
