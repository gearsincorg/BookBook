#pragma once
#include <string>
#include "config.h"
#include "esp_err.h"

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

std::string preferred_authors_json();
std::string preferred_genres_json();
esp_err_t add_author(const Config& c, const std::string& name, bool favorite);
esp_err_t add_genre(const Config& c, const std::string& genre);

// What add_to_bookshelf needs to tell the model (persona rule: ask once per new author, propose a genre).
struct AddInfo {
    bool author_known = false;
    std::string genres_json = "[]";
};
AddInfo add_info(const std::string& author);  // read-only

// Automatic reading-history logging when a book is really added to / removed from the bookshelf.
esp_err_t log_added(const Config& c, const std::string& title, const std::string& author, const std::string& bookshare_id);
esp_err_t log_removed(const Config& c, const std::string& bookshare_id);

}  // namespace memory
