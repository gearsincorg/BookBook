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
};

struct Shelf {
    int loan_count = 0;  // books + music + braille, out of kLoanCap
    int periodical_count = 0;
    std::vector<ShelfItem> books;
};

// Sets the credentials and logs in (3-step handshake). On failure `error` is a short reason.
esp_err_t login(const std::string& user, const std::string& password, std::string* error = nullptr);
bool logged_in();

// `type` is the portal's own dropdown value: "Book", "Picture Book", "Magazine", ...
esp_err_t search(const std::string& keyword, SearchResult& out, int limit = 20, const char* type = "Book");
esp_err_t bookshelf(Shelf& out);
esp_err_t request_list(std::vector<ShelfItem>& out, int* total = nullptr);

}  // namespace va
