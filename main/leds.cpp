#include "leds.h"

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "leds";

namespace {

// Brightness: the spinner head is bright; steady "status" colours (green, blue, red) are low so they are not glaring.
struct Colour {
    unsigned char r, g, b;
};
constexpr Colour kYellowBright = {120, 80, 0};
constexpr Colour kBlueBright = {0, 0, 130};
constexpr Colour kGreenLow = {0, 18, 0};
constexpr Colour kBlueLow = {0, 0, 22};
constexpr Colour kRedLow = {24, 0, 0};
constexpr Colour kYellowLow = {30, 20, 0};  // the single "update available" LED beside the green

constexpr int kFlashPeriodMs = 500;  // 2 Hz: the whole ring on for a quarter of a second, then off
constexpr int kSpinPeriodMs = 2000;  // one lap per two seconds (slower hides small Wi-Fi-induced hiccups)
// The head, then a 3-LED tail of diminishing intensity (share of the head's brightness).
constexpr float kTrail[] = {1.0f, 0.45f, 0.20f, 0.08f};

enum class Mode { Solid, Spin, Flash };

struct State {
    Mode mode = Mode::Solid;
    Colour colour = {0, 0, 0};
    bool accent = false;  // Solid only: LED 0 in kYellowLow (update available)
    unsigned serial = 0;  // bumped on every change so the task knows to redraw
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
State s_state;
TaskHandle_t s_task;

void set(Mode mode, Colour c, const char* what, bool accent = false) {
    portENTER_CRITICAL(&s_mux);
    unsigned next = s_state.serial + 1;
    bool changed = s_state.mode != mode || s_state.accent != accent || s_state.colour.r != c.r || s_state.colour.g != c.g || s_state.colour.b != c.b;
    s_state = {mode, c, accent, next};
    portEXIT_CRITICAL(&s_mux);
    if (changed) ESP_LOGI(TAG, "%s", what);
}

void draw_spinner(const State& s, int head) {
    const int n = board::led_count();
    for (int i = 0; i < n; i++) board::set_pixel(i, 0, 0, 0);
    for (int k = 0; k < 4; k++) {
        int idx = ((head - k) % n + n) % n;  // the tail trails behind the head
        board::set_pixel(idx, static_cast<uint8_t>(s.colour.r * kTrail[k]), static_cast<uint8_t>(s.colour.g * kTrail[k]),
                         static_cast<uint8_t>(s.colour.b * kTrail[k]));
    }
    board::show();
}

void animation_task(void*) {
    unsigned drawn = 0;
    int last_head = -1;
    while (true) {
        State s;
        portENTER_CRITICAL(&s_mux);
        s = s_state;
        portEXIT_CRITICAL(&s_mux);

        if (s.mode == Mode::Flash) {
            int64_t ms = esp_timer_get_time() / 1000;
            int phase = static_cast<int>(ms % kFlashPeriodMs) * 2 / kFlashPeriodMs;  // 0 = on, 1 = off
            if (s.serial != drawn || phase != last_head) {
                board::set_leds(phase == 0 ? s.colour.r : 0, phase == 0 ? s.colour.g : 0, phase == 0 ? s.colour.b : 0);
                last_head = phase;
                drawn = s.serial;
            }
        } else if (s.mode == Mode::Spin) {
            const int n = board::led_count();
            int64_t ms = esp_timer_get_time() / 1000;
            int head = static_cast<int>((ms % kSpinPeriodMs) * n / kSpinPeriodMs);
            if (s.serial != drawn || head != last_head) {
                static int64_t last_draw_ms = 0;
                if (last_draw_ms && ms - last_draw_ms > 200) ESP_LOGW(TAG, "spinner stalled for %d ms", static_cast<int>(ms - last_draw_ms));
                last_draw_ms = ms;
                draw_spinner(s, head);
                last_head = head;
                drawn = s.serial;
            }
        } else if (s.serial != drawn) {
            if (s.accent) {
                for (int i = 0; i < board::led_count(); i++) {
                    const Colour& c = i == 0 ? kYellowLow : s.colour;
                    board::set_pixel(i, c.r, c.g, c.b);
                }
                board::show();
            } else {
                board::set_leds(s.colour.r, s.colour.g, s.colour.b);
            }
            last_head = -1;
            drawn = s.serial;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

}  // namespace

namespace leds {

void start() {
    // Priority above the audio and turn tasks, so generating a tone cannot make the spinner stutter.
    if (!s_task) xTaskCreate(animation_task, "leds", 3072, nullptr, 8, &s_task);
}

void solid(unsigned char r, unsigned char g, unsigned char b) { set(Mode::Solid, {r, g, b}, "solid"); }

void boot_sequence() {
    solid(40, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    solid(0, 40, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    solid(0, 0, 40);
    vTaskDelay(pdMS_TO_TICKS(400));
    waiting();
}

void waiting() { set(Mode::Spin, kYellowBright, "spinning yellow: waiting"); }
void ready(bool update_available) {
    set(Mode::Solid, kGreenLow, update_available ? "green with one yellow LED: ready, update available" : "green: ready for touch-to-talk",
        update_available);
}
void listening() { set(Mode::Solid, kBlueLow, "blue: listening"); }
void thinking() { set(Mode::Spin, kBlueBright, "spinning blue: waiting for the answer"); }
void speaking() { set(Mode::Solid, kRedLow, "red: speaking"); }
void updating() { set(Mode::Flash, kYellowBright, "flashing yellow: downloading an update"); }

}  // namespace leds
