// Phase 0-2 bring-up: board IO, Wi-Fi, web setup page, Azure speech test.
#include <atomic>
#include <memory>

#include "audio.h"
#include "brain.h"
#include "azure.h"
#include "board.h"
#include "config.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "leds.h"
#include "esp_log.h"
#include "memory.h"
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

// Ready (green) once online, otherwise still waiting (spinning yellow), for example before Wi-Fi is up.
static void show_idle_state() {
    if (wifi::connected()) leds::ready();
    else leds::waiting();
}

// Polled by the speech player so a button press stops a long reading.
static bool key_cancel() { return board::key_pressed(board::Key::Key1); }

static void say(const Config& c, const std::string& text) {
    thinking::stop();  // the real answer is ready: waiting sounds end immediately
    if (c.azure_key.empty()) return;
    leds::speaking();  // low red while it is spoken
    ESP_LOGI(TAG, "say: %s", text.c_str());
    azure::speak(c.azure_region.c_str(), c.azure_key.c_str(), text.c_str(), key_cancel);
}

// One push-to-talk turn. The slow part (speech-to-text, then the librarian) runs on its own task so the main
// loop can keep watching the pad: a touch while it waits ABORTS the wait (like a touch stops an answer being
// spoken), and a deadline stops it waiting forever. The task owns its data through a shared_ptr, so an
// abandoned turn can finish and clean up by itself; it checks `cancel` between steps and drops its result.
struct Turn {
    Config cfg;
    std::vector<int16_t> pcm;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::string spoken;  // what to say back
};

static void turn_task(void* arg) {
    auto* holder = static_cast<std::shared_ptr<Turn>*>(arg);
    std::shared_ptr<Turn> t = *holder;
    delete holder;
    const Config& c = t->cfg;

    mic::Stats stats;
    mic::process(t->pcm, &stats);
    ESP_LOGI(TAG, "recorded %u ms, rms %d, peak %d", static_cast<unsigned>(t->pcm.size() * 1000 / mic::kSampleRateHz),
             stats.rms, stats.peak);
    if (!wifi::connected()) {
        t->spoken = "I am not connected to the internet.";
    } else {
        std::string text, status;
        esp_err_t err = c.azure_key.empty() ? ESP_ERR_INVALID_STATE
                                            : azure::transcribe(c.azure_region.c_str(), c.azure_key.c_str(), t->pcm.data(),
                                                                t->pcm.size(), text, &status, "en-AU", &t->cancel);
        ESP_LOGI(TAG, "heard: \"%s\" (%s)", text.c_str(), status.c_str());
        if (t->cancel) {
            // abandoned: say nothing
        } else if (err != ESP_OK) {
            t->spoken = "I could not reach the speech service.";
        } else if (text.empty()) {
            t->spoken = "I did not hear anything I could understand.";
        } else {
            std::string reply;
            brain::respond(c, text, reply, &t->cancel);  // always yields something speakable, even on failure
            if (!t->cancel) {
                ESP_LOGI(TAG, "reply: %s", reply.c_str());
                t->spoken = reply;
            }
        }
    }
    t->done = true;
    vTaskDelete(nullptr);
}

// Push-to-talk turn: transcribe what was recorded, ask the librarian brain (Claude with library tools), and
// speak the answer. Returns true if the pad is still touched when it ends because the touch that aborted it
// (or stopped the answer) is still down: the caller must ignore that press's release.
static bool handle_utterance(const Config& c, std::vector<int16_t>& pcm) {
    constexpr int kTurnDeadlineMs = 45000;  // give up rather than wait for ever
    constexpr int kAbortGuardMs = 300;      // ignore pad bounce right after letting go
    ESP_LOGI(TAG, "turn start: free internal heap %u (largest %u), PSRAM %u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    leds::thinking();   // spinning blue until the answer comes back
    thinking::start();  // confirmation beep, then soft chimes

    auto turn = std::make_shared<Turn>();
    turn->cfg = c;
    turn->pcm = std::move(pcm);
    auto* holder = new std::shared_ptr<Turn>(turn);
    if (xTaskCreate(turn_task, "turn", 16384, holder, 5, nullptr) != pdPASS) {
        delete holder;
        thinking::stop();
        say(c, "Sorry, I ran out of memory. Please try again.");
        return key_cancel();
    }

    const int64_t start_ms = esp_timer_get_time() / 1000;
    bool aborted = false, timed_out = false;
    while (!turn->done) {
        const int64_t waited = esp_timer_get_time() / 1000 - start_ms;
        if (waited > kAbortGuardMs && board::key_pressed(board::Key::Key1)) {
            aborted = true;
            break;
        }
        if (waited > kTurnDeadlineMs) {
            timed_out = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (aborted || timed_out) turn->cancel = true;  // the task stops at its next step and discards its result
    thinking::stop();
    if (aborted) {
        ESP_LOGI(TAG, "turn aborted by a touch");
        return true;
    }
    if (timed_out) {
        ESP_LOGW(TAG, "turn timed out after %d ms", kTurnDeadlineMs);
        say(c, "Sorry, that took too long, so I stopped. If you were changing your lists, please ask me to check them.");
        return key_cancel();
    }
    say(c, turn->spoken);
    return key_cancel();
}

// Start-up preparation that only speeds up the first request, so it must not keep the member waiting: read
// the shared memory file, and sign in to the library. Runs in the background after the greeting.
static void warmup_task(void* arg) {
    const Config& cfg = *static_cast<const Config*>(arg);
    int64_t t0 = esp_timer_get_time();
    if (memory::configured(cfg)) {
        esp_err_t m = memory::refresh(cfg);  // loads on first use
        memory::Counts n = memory::counts();
        ESP_LOGI(TAG, "memory: %s (%d preferences, %d authors, %d genres, %d books read, %d on hold)", esp_err_to_name(m),
                 n.preferences, n.authors, n.genres, n.history, n.standby);
    }
    if (!cfg.va_user.empty()) {
        std::string why;
        ESP_LOGI(TAG, "library sign-in at startup: %s",
                 esp_err_to_name(va::ensure_logged_in(cfg.va_user, cfg.va_password, &why)));
    }
    ESP_LOGI(TAG, "warm-up finished in %d ms", static_cast<int>((esp_timer_get_time() - t0) / 1000));
    vTaskDelete(nullptr);
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

    // Boot: red, green, blue, then spinning yellow (waiting) until the device is ready for touch-to-talk.
    leds::start();
    leds::boot_sequence();

    // Keep hands off the touch pad while powering up: it calibrates against the untouched pad (a high
    // calibration recovers by itself, see touch.cpp). Setup is reached over the normal Wi-Fi
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
            leds::speaking();  // low red while the intro is spoken, like any spoken answer
            azure::speak(cfg.azure_region.c_str(), cfg.azure_key.c_str(),
                         "Hi Bruce. Use Press-To-talk to make changes to your library.");
        }
        // Memory and library sign-in prepare the first request; they run in the background so the LEDs go
        // blue (ready for press-to-talk) as soon as the greeting has finished.
        xTaskCreate(warmup_task, "warmup", 12288, &cfg, 4, nullptr);
    }
    show_idle_state();
    ESP_LOGI(TAG, "ready: press-to-talk is available");

    // Button: push-to-talk. Recording runs while it is held (up to 15 s); on release the recording is
    // transcribed and answered. A press shorter than kMinTalkMs is a bump, not speech: ignored.
    constexpr int kMinTalkMs = 700;
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
            leds::listening();  // low blue while the pad is touched
            down_since_ms = now_ms;
            talk_ended = false;
            pcm.clear();
            pcm.reserve(static_cast<size_t>(mic::kSampleRateHz) * kMaxTalkMs / 1000);  // a finished turn takes the buffer
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
                show_idle_state();
            }
        } else if (!down && was_down) {
            ESP_LOGI(TAG, "Key1 up after %d ms", static_cast<int>(now_ms - down_since_ms));
            mic::stop();
            if (ignore_release) {
                ignore_release = false;  // this release ends a press that must not act
            } else if (!talk_ended && now_ms - down_since_ms >= kMinTalkMs) {
                Config c = config::get();
                if (handle_utterance(c, pcm)) ignore_release = true;  // spinning blue, then red while it speaks
            }
            show_idle_state();  // back to green, ready for the next touch
        }
        was_down = down;
        if (!down && wifi::connected() != last_online) {
            last_online = wifi::connected();
            show_idle_state();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
