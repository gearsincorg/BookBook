#pragma once
#include <atomic>
#include <string>
#include "config.h"
#include "esp_err.h"

// The librarian's brain: a Claude conversation with library tools, modelled on Bookworm's
// LibrarianOrchestrator (turn loop: user text -> Claude -> run any tool calls -> feed results back ->
// repeat, up to 8 rounds of tool calls and then one last round without tools) and its persona rules.
//
// Tools (kToolsJson in brain.cpp): library (search, bookshelf, request list, subscriptions), memory
// (preferences, authors, genres, books list and ratings, the On Hold list, reading profile), and the device
// itself (updates, version, setup network, volume). Library changes honour Config::dry_run (practice mode) and
// are verified by re-reading the shelf or list. Memory tools read and write the shared memory file (see
// memory.h). The persona rules are documented in docs/conversation-rules.md.
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
