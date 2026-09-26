#pragma once
#include <atomic>
#include <string>
#include "config.h"
#include "esp_err.h"

// The librarian's brain: a Claude conversation with library tools, modelled on Bookworm's
// LibrarianOrchestrator (turn loop: user text -> Claude -> run any tool calls -> feed results back ->
// repeat, capped at 5 round trips) and its persona rules.
//
// Tools: search_library, get_bookshelf, get_request_list, get_reading_profile, add_to_on_hold,
// get_on_hold_list, remove_from_on_hold, add_to_bookshelf, remove_from_bookshelf,
// add_to_request_list. Changes honour Config::dry_run (practice mode) and are verified by re-reading the
// shelf. Memory tools (preferences, favourite authors and genres, reading history and ratings) read and
// write the shared memory file (see memory.h).
namespace brain {

// Answers one spoken request. `reply` is always set to something speakable, even on failure (a short
// apology, never a raw error). Returns ESP_OK if it came from Claude. Thread-safe; turns are serialised.
// `cancel`, if given, is checked between steps: once it is set the turn is abandoned (history rolled back,
// reply left empty, ESP_ERR_INVALID_STATE). The step in progress still has to return first.
esp_err_t respond(const Config& cfg, const std::string& user_text, std::string& reply,
                  const std::atomic<bool>* cancel = nullptr);

// Forgets the conversation so far (also happens automatically after 10 idle minutes).
void reset();

}  // namespace brain
