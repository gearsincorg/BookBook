#pragma once
#include <string>
#include "config.h"
#include "esp_err.h"

// The librarian's brain: a Claude conversation with library tools, modelled on Bookworm's
// LibrarianOrchestrator (turn loop: user text -> Claude -> run any tool calls -> feed results back ->
// repeat, capped at 5 round trips) and its persona rules.
//
// Tools today (read-only): search_library, get_bookshelf, get_request_list. Adding/removing books and
// memory come later.
namespace brain {

// Answers one spoken request. `reply` is always set to something speakable, even on failure (a short
// apology, never a raw error). Returns ESP_OK if it came from Claude. Thread-safe; turns are serialised.
esp_err_t respond(const Config& cfg, const std::string& user_text, std::string& reply);

// Forgets the conversation so far (also happens automatically after 10 idle minutes).
void reset();

}  // namespace brain
