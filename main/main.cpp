// Phase 0-2 bring-up: board IO, Wi-Fi, web setup page, Azure speech test.
#include "audio.h"
#include "azure.h"
#include "board.h"
#include "config.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mic.h"
#include <string>
#include <vector>
#include "thinking.h"
#include "va.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "webconfig.h"
#include "wifi.h"

static const char* TAG = "bookbook";

static void show_idle_state() {
    if (wifi::connected()) {
        board::set_leds(0, 0, 20);   // blue: online
    } else if (!wifi::ap_ssid().empty()) {
        board::set_leds(25, 0, 25);  // purple: setup network is up
    } else {
        board::set_leds(20, 10, 0);  // amber: connecting
    }
}

// Polled by the speech player so a button press stops a long reading.
static bool key_cancel() { return board::key_pressed(board::Key::Key1); }

static void say(const Config& c, const std::string& text) {
    thinking::stop();  // the real answer is ready: waiting sounds end immediately
    if (c.azure_key.empty()) return;
    ESP_LOGI(TAG, "say: %s", text.c_str());
    azure::speak(c.azure_region.c_str(), c.azure_key.c_str(), text.c_str(), key_cancel);
}

// Catalogue authors look like "Silva, Daniel, 1960-": drop the dates, put the first name first.
static std::string spoken_author(const std::string& raw) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t comma = raw.find(',', start);
        std::string p = raw.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        while (!p.empty() && p.front() == ' ') p.erase(p.begin());
        while (!p.empty() && p.back() == ' ') p.pop_back();
        bool has_digit = p.find_first_of("0123456789") != std::string::npos;
        if (!p.empty() && !has_digit) parts.push_back(p);  // dates like "1960-" or "1931-2020" contain digits
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (parts.size() == 2) return parts[1] + " " + parts[0];
    std::string joined;
    for (const auto& p : parts) joined += (joined.empty() ? "" : ", ") + p;
    return joined;
}

static std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

// Short press: fetch the library bookshelf and read it aloud. Never speaks raw errors.
// Returns true if playback was interrupted by a button press.
[[maybe_unused]] static bool read_bookshelf_aloud(const Config& c) {
    board::set_leds(0, 25, 25);  // cyan: working
    if (!wifi::connected()) {
        say(c, "I am not connected to the internet.");
        return false;
    }
    thinking::start();  // confirmation beep, then soft chimes while we wait (Bookworm's strategy)
    if (!va::logged_in()) {
        std::string why;
        if (va::login(c.va_user, c.va_password, &why) != ESP_OK) {
            say(c, why == "no library login saved" ? "There is no library login saved yet. Please use the setup page."
                                                   : "I could not sign in to the library.");
            return false;
        }
    }
    va::Shelf shelf;
    if (va::bookshelf(shelf) != ESP_OK) {
        say(c, "I could not get your bookshelf.");
        return false;
    }

    std::string text;
    if (shelf.books.empty()) {
        text = "Your bookshelf is empty.";
    } else {
        text = "Your bookshelf has " + plural(static_cast<int>(shelf.books.size()), "book", "books") + ". ";
        int n = 0;
        for (const auto& b : shelf.books) {
            std::string title = b.title;
            while (!title.empty() && (title.back() == '.' || title.back() == ' ')) title.pop_back();
            text += "Number " + std::to_string(++n) + ", " + title;
            std::string who = spoken_author(b.author);
            if (!who.empty()) text += ", by " + who;
            text += ". ";
        }
    }
    int free_slots = va::kLoanCap - shelf.loan_count;
    if (free_slots > 0) text += "You have " + plural(free_slots, "free place", "free places") + " left.";
    else text += "Your bookshelf is full.";

    board::set_leds(0, 0, 20);
    say(c, text);
    return key_cancel();  // pressed during speech: caller swallows the matching release
}

// Push-to-talk turn: transcribe what was recorded and answer it. For now the answer just says back
// what was heard (Bookworm's Phase 2 echo test); the Claude conversation replaces this.
// Returns true if the user pressed the button during the answer (that press stops it).
static bool handle_utterance(const Config& c, std::vector<int16_t>& pcm) {
    mic::Stats stats;
    mic::process(pcm, &stats);
    ESP_LOGI(TAG, "recorded %u ms, rms %d, peak %d", static_cast<unsigned>(pcm.size() * 1000 / mic::kSampleRateHz),
             stats.rms, stats.peak);
    board::set_leds(0, 25, 25);  // cyan: working
    if (!wifi::connected()) {
        say(c, "I am not connected to the internet.");
        return false;
    }
    thinking::start();  // confirmation beep, then soft chimes until the answer is ready
    std::string text, status;
    esp_err_t err = c.azure_key.empty() ? ESP_ERR_INVALID_STATE
                                        : azure::transcribe(c.azure_region.c_str(), c.azure_key.c_str(),
                                                            pcm.data(), pcm.size(), text, &status);
    ESP_LOGI(TAG, "heard: \"%s\" (%s)", text.c_str(), status.c_str());
    board::set_leds(0, 0, 20);
    if (err != ESP_OK) say(c, "I could not reach the speech service.");
    else if (text.empty()) say(c, "I did not hear anything I could understand.");
    else say(c, "I heard: " + text);
    return key_cancel();
}

extern "C" void app_main() {
    config::load();
    Config cfg = config::get();

    if (board::init() != ESP_OK) {
        ESP_LOGW(TAG, "board init incomplete (not a Waveshare audio board?); continuing");
    }
    // Microphone first: PDM receive needs I2S0 (the speaker is pinned to I2S1 either way).
    if (mic::init() != ESP_OK) ESP_LOGW(TAG, "microphone unavailable");
    if (audio::init() != ESP_OK) ESP_LOGW(TAG, "audio output unavailable");
    audio::set_volume(cfg.volume);

    // LED colour-order check: expect red, green, blue in turn.
    board::set_leds(40, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    board::set_leds(0, 40, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    board::set_leds(0, 0, 40);
    vTaskDelay(pdMS_TO_TICKS(400));
    board::set_leds(20, 10, 0);

    // Note: Key1 cannot be used at boot on the XIAO stand-in (BOOT is GPIO0, a strapping pin:
    // holding it at reset enters the ROM downloader). Setup is reached over the normal Wi-Fi
    // (http://bookbook.local/); the setup network starts by itself when Wi-Fi is missing or fails.
    bool have_wifi = !cfg.wifi_ssid.empty();

    ESP_ERROR_CHECK(wifi::init());
    webconfig::start(/*trust_setup_ap=*/!have_wifi);

    bool online = false;
    if (have_wifi) {
        online = wifi::start_sta(cfg.wifi_ssid.c_str(), cfg.wifi_password.c_str(), 20000) == ESP_OK;
        ESP_LOGI(TAG, "wifi: %s", online ? "connected" : "failed, starting setup network");
    }
    if (!online) {
        wifi::enable_ap();
        webconfig::start_captive_dns();
    }

    if (online) {
        ESP_LOGI(TAG, "time sync: %s", esp_err_to_name(wifi::sync_time(15000)));
        ESP_LOGI(TAG, "setup page: http://%s/ or http://bookbook.local/", wifi::ip().c_str());
        if (!cfg.azure_key.empty()) {
            azure::speak(cfg.azure_region.c_str(), cfg.azure_key.c_str(),
                         "Hello. This is Book Book, speaking from an E S P 32.");
        }
        // Sign in to the library now so the first button press is fast.
        if (!cfg.va_user.empty()) {
            std::string why;
            ESP_LOGI(TAG, "library sign-in at startup: %s",
                     esp_err_to_name(va::login(cfg.va_user, cfg.va_password, &why)));
        }
    }
    show_idle_state();

    // Button: push-to-talk. Recording runs while it is held (up to 15 s); on release the recording is
    // transcribed and answered. A press shorter than kMinTalkMs is a bump, not speech: ignored.
    constexpr int kMinTalkMs = 400;
    constexpr int kMaxTalkMs = 15000;
    bool was_down = false;
    bool ignore_release = false;  // a press that only cancelled a reading/answer, or an over-long talk
    bool talk_ended = false;
    int64_t down_since_ms = 0;
    bool last_online = wifi::connected();
    std::vector<int16_t> pcm;
    pcm.reserve(static_cast<size_t>(mic::kSampleRateHz) * kMaxTalkMs / 1000);
    while (true) {
        bool down = board::key_pressed(board::Key::Key1);
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (down && !was_down) {
            ESP_LOGI(TAG, "Key1 down");
            board::set_leds(0, 60, 0);
            down_since_ms = now_ms;
            talk_ended = false;
            pcm.clear();
            if (!ignore_release) mic::start();  // a cancelling press must not record
        } else if (down && !talk_ended && !ignore_release) {
            mic::read(pcm, 20);
            if (now_ms - down_since_ms >= kMaxTalkMs) {
                // Too long: answer what we have now; the eventual release is then ignored.
                ESP_LOGI(TAG, "talk limit reached");
                talk_ended = true;
                mic::stop();
                Config c = config::get();
                handle_utterance(c, pcm);
                ignore_release = true;
            }
        } else if (!down && was_down) {
            ESP_LOGI(TAG, "Key1 up after %d ms", static_cast<int>(now_ms - down_since_ms));
            mic::stop();
            if (ignore_release) {
                ignore_release = false;  // this release ends a press that must not act
            } else if (!talk_ended && now_ms - down_since_ms >= kMinTalkMs) {
                Config c = config::get();
                if (handle_utterance(c, pcm)) ignore_release = true;
            }
            show_idle_state();
        }
        was_down = down;
        if (!down && wifi::connected() != last_online) {
            last_online = wifi::connected();
            show_idle_state();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
