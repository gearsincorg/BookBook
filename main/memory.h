#pragma once
#include <string>
#include "config.h"
#include "esp_err.h"
#include "va.h"

// Cross-session memory, shared with Bookworm. One JSON document (memory.json) in Azure Blob storage,
// in exactly Bookworm's BookwormMemory format (PascalCase: ExplicitPreferences, ConversationNotes,
// LastSessionSummary, ReadingHistory, PreferredAuthors, PreferredGenres), so both apps read and write the
// same file. Access is a container-scoped SAS URL (Config::memory_url): the storage account key never
// goes on the device. Writes use the blob's ETag (If-Match) for optimistic concurrency; on a conflict the
// document is re-read and the change re-applied once.
//
// Everything here is thread-safe. Without a memory URL every write returns ESP_ERR_INVALID_STATE.
namespace memory {

bool configured(const Config& c);
esp_err_t load(const Config& c);  // (re)read from Azure; a missing blob means empty memory
bool loaded();

struct Counts {
    int preferences = 0, notes = 0, authors = 0, genres = 0, history = 0;
};
Counts counts();

// Compact JSON of what is worth having in the system prompt (excludes the reading history, which can
// grow large and is searched with a tool instead).
std::string prompt_snapshot();

std::string recall();  // JSON of explicit preferences, notes, last session summary
esp_err_t remember(const Config& c, const std::string& note);

std::string search_history(const std::string& query);  // JSON array of matching entries
// Sets a 1-5 rating on the one title matching `title` (partial, case-insensitive). ESP_ERR_NOT_FOUND if
// none, ESP_ERR_INVALID_SIZE if several match (count in *matches).
esp_err_t rate(const Config& c, const std::string& title, int rating, std::string* matched_title, int* matches);

// The books list is the reading history: every book the member knows, some of them favourites (rated 4 or 5;
// Bookworm's format has no separate flag). A book gets on it when the member adds it (add_book) or when it is
// added to the bookshelf (log_added); it is never listed twice (matched by catalogue id, else exact title).
//
// add_book puts a book on the list even if it was never on the bookshelf (no add/remove dates). If it is
// already there it is updated. `favorite` true makes it a favourite (5 stars unless already rated 4+);
// false takes a favourite off the favourites but keeps the book on the list (a rating below 4 is kept).
// `created` says whether a new entry was made.
esp_err_t add_book(const Config& c, const std::string& title, const std::string& author, const std::string& bookshare_id,
                   bool favorite, bool* created);

// Removes a book from the list entirely (title exactly, ignoring case, or a unique partial match).
// ESP_ERR_NOT_FOUND if none match, ESP_ERR_INVALID_SIZE if several do (count in *matches).
esp_err_t remove_book(const Config& c, const std::string& title, std::string* matched_title, int* matches);

// A small, deterministic taste summary (Bookworm's ReaderProfile idea): the most frequent authors across the
// bookshelf and the books list, favourite authors and books, what is on the shelf now, and the books list
// (so already-owned or already-read titles are not suggested again). Genre and theme reasoning is left to
// the model. Works without memory (then only the shelf is used).
std::string reading_profile(const va::Shelf& shelf);

std::string preferred_authors_json();std::string preferred_authors_json();
std::string preferred_genres_json();
esp_err_t add_author(const Config& c, const std::string& name, bool favorite);
esp_err_t add_genre(const Config& c, const std::string& genre);
// Removes an author from the preferred authors. Matches the name exactly (ignoring case and the catalogue
// format "Silva, Daniel, 1960-") or, failing that, partially: ESP_ERR_NOT_FOUND if nothing matches,
// ESP_ERR_INVALID_SIZE if several do (count in *matches). `matched_name` is the author that was removed.
esp_err_t remove_author(const Config& c, const std::string& name, std::string* matched_name, int* matches);

// What add_to_bookshelf needs to tell the model (persona rule: ask once per new author, propose a genre).
struct AddInfo {
    bool author_known = false;
    std::string genres_json = "[]";
};
AddInfo add_info(const std::string& author);  // read-only

// Automatic reading-history logging when a book is really added to / removed from the bookshelf.
// Also puts the book's author(s) on the preferred-authors list (not as favourites) if they are not there
// already; `author_added` reports whether that created a new entry.
esp_err_t log_added(const Config& c, const std::string& title, const std::string& author, const std::string& bookshare_id,
                    bool* author_added = nullptr);
esp_err_t log_removed(const Config& c, const std::string& bookshare_id);

}  // namespace memory
