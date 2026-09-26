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

constexpr int kSpinPeriodMs = 2000;  // one lap per two seconds (slower hides small Wi-Fi-induced hiccups)
// The head, then a 3-LED tail of diminishing intensity (share of the head's brightness).
constexpr float kTrail[] = {1.0f, 0.45f, 0.20f, 0.08f};

struct State {
    bool spin = false;
    Colour colour = {0, 0, 0};
    unsigned serial = 0;  // bumped on every change so the task knows to redraw
};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
State s_state;
TaskHandle_t s_task;

void set(bool spin, Colour c, const char* what) {
    portENTER_CRITICAL(&s_mux);
    unsigned next = s_state.serial + 1;
    bool changed = s_state.spin != spin || s_state.colour.r != c.r || s_state.colour.g != c.g || s_state.colour.b != c.b;
    s_state = {spin, c, next};
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

        if (s.spin) {
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
            board::set_leds(s.colour.r, s.colour.g, s.colour.b);
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

void solid(unsigned char r, unsigned char g, unsigned char b) { set(false, {r, g, b}, "solid"); }

void boot_sequence() {
    solid(40, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    solid(0, 40, 0);
    vTaskDelay(pdMS_TO_TICKS(400));
    solid(0, 0, 40);
    vTaskDelay(pdMS_TO_TICKS(400));
    waiting();
}

void waiting() { set(true, kYellowBright, "spinning yellow: waiting"); }
void ready() { set(false, kGreenLow, "green: ready for touch-to-talk"); }
void listening() { set(false, kBlueLow, "blue: listening"); }
void thinking() { set(true, kBlueBright, "spinning blue: waiting for the answer"); }
void speaking() { set(false, kRedLow, "red: speaking"); }

}  // namespace leds
