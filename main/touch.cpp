// Capacitive touch presence sensing (ESP32-S3 built-in touch peripheral).
//
// Ported from PhilbotSays (firmware/components/touch_sense/touch_sense.c), where it works well on the
// same PCB. The logic and every constant are unchanged; only the plumbing differs: instead of a
// present/not-present callback this exposes touch::present(), which board::key_pressed() reads, and the
// optional dev pushbutton is handled in board.cpp.
//
// The legacy touch API (deprecated in favor of driver/touch_sens.h, but still fully functional) is used
// because it is already reachable through the "driver" component. CONFIG_TOUCH_SUPPRESS_DEPRECATE_WARN
// silences the deprecation #warning (see sdkconfig.defaults).
#include "touch.h"

#include <cinttypes>

#include "driver/touch_pad.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_BOOKBOOK_BOARD_XIAO_ESP32S3

static const char* TAG = "touch";

#define TOUCH_PIN CONFIG_BOOKBOOK_TOUCH_PIN

#define POLL_MS 20

#define TOUCH_BASELINE_SAMPLES 10
#define TOUCH_BASELINE_SAMPLE_MS 10

// A touch is declared active once the raw reading has risen this fraction
// above the untouched baseline sampled at boot. Only a rise counts - a
// touch reliably increases this pad's raw reading (hardware-confirmed
// 2026-09-23), so a large drop below baseline is never mistaken for a
// touch. That matters most right as a stuck-high baseline (e.g. one
// baked in by holding the pad through boot) corrects itself back down -
// raw crossing far below it used to register as a spurious touch on the
// way down. There's plenty of room here: hardware traces showed real
// touches at 3-4x+ baseline (deltas of 40000-110000+) while idle noise
// came within 2% of the old 0.15 threshold (a logged delta of 4440
// against a ~4505 trigger at that baseline) - doubled to keep well clear
// of noise while staying far below any real touch. Lower this ratio if a
// surface-covered pad isn't registering; raise it further if it still
// free-runs from noise.
// BookBook: lowered from PhilbotSays' 0.30 to 0.20 in two 5-point steps (owner, 2026-09-26) because 0.30 asked
// for too firm a touch. CAUTION: PhilbotSays measured idle noise up to ~14.7% of baseline, so the release point
// (0.20 - 0.05 = 0.15) now sits right at the noise ceiling. If the pad free-runs or sticks on, raise this.
#define TOUCH_ACTIVE_DELTA_RATIO 0.20f

// Once active, the delta must fall back below (ACTIVE - HYSTERESIS) before
// touch_is_active() reports inactive again - so a reading hovering right
// at the active threshold doesn't chatter on/off.
#define TOUCH_HYSTERESIS_RATIO 0.05f

// If this many consecutive valid, non-zero readings all land below the
// current baseline, the baseline is reset to their average - lets it
// track a genuine downward drift (e.g. temperature/humidity) instead of
// staying pinned to whatever was sampled once at boot forever. Any
// reading that breaks the streak (a failed read, a zero reading, or one
// at/above the current baseline) resets the count - it has to be 10 in a
// row, not 10 out of some larger window. This is also what recovers from
// a baseline calibrated too HIGH (pad held or wet at power-up).
#define TOUCH_LOW_BASELINE_STREAK 10

static TaskHandle_t s_task;
static uint32_t s_baseline;
static bool s_touch_active;
static uint32_t s_low_streak_sum;
static int s_low_streak_count;
static volatile bool s_present;  // debounced result, read by other tasks

static bool touch_is_active(void) {
    uint32_t raw = 0;
    if (touch_pad_read_raw_data(static_cast<touch_pad_t>(TOUCH_PIN), &raw) != ESP_OK) {
        s_low_streak_count = 0;
        s_low_streak_sum = 0;
        return s_touch_active;  // keep last known state on a transient read error
    }

    if (raw != 0 && raw < s_baseline) {
        s_low_streak_sum += raw;
        if (++s_low_streak_count >= TOUCH_LOW_BASELINE_STREAK) {
            s_baseline = s_low_streak_sum / TOUCH_LOW_BASELINE_STREAK;
            ESP_LOGI(TAG, "baseline drifted down, adapted to %" PRIu32, s_baseline);
            s_low_streak_count = 0;
            s_low_streak_sum = 0;
        }
    } else {
        s_low_streak_count = 0;
        s_low_streak_sum = 0;
    }

    uint32_t delta = raw > s_baseline ? raw - s_baseline : 0;
    float ratio = s_touch_active ? (TOUCH_ACTIVE_DELTA_RATIO - TOUCH_HYSTERESIS_RATIO) : TOUCH_ACTIVE_DELTA_RATIO;
    s_touch_active = delta > static_cast<uint32_t>(s_baseline * ratio);
    return s_touch_active;
}

static void sense_task(void*) {
    ESP_ERROR_CHECK(touch_pad_init());
    ESP_ERROR_CHECK(touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER));
    ESP_ERROR_CHECK(touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V));
    ESP_ERROR_CHECK(touch_pad_config(static_cast<touch_pad_t>(TOUCH_PIN)));
    ESP_ERROR_CHECK(touch_pad_fsm_start());

    // Sample the untouched pad a few times so the active delta is set
    // relative to this pad/wiring's real baseline, not a guessed constant.
    uint64_t sum = 0;
    for (int i = 0; i < TOUCH_BASELINE_SAMPLES; i++) {
        vTaskDelay(pdMS_TO_TICKS(TOUCH_BASELINE_SAMPLE_MS));
        uint32_t raw = 0;
        ESP_ERROR_CHECK(touch_pad_read_raw_data(static_cast<touch_pad_t>(TOUCH_PIN), &raw));
        sum += raw;
    }
    s_baseline = static_cast<uint32_t>(sum / TOUCH_BASELINE_SAMPLES);
    ESP_LOGI(TAG, "touch pad GPIO%d calibrated, baseline %" PRIu32, TOUCH_PIN, s_baseline);

    bool present = false;
    bool last_read = false;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        bool read = touch_is_active();

        // Simple debounce: only act once the same level has held for two
        // consecutive polls.
        if (read != last_read) {
            last_read = read;
            continue;
        }
        if (read == present) {
            continue;
        }
        present = read;
        s_present = present;
    }
}

namespace touch {

esp_err_t start() {
    if (s_task != nullptr) return ESP_ERR_INVALID_STATE;
    BaseType_t ok = xTaskCreate(sense_task, "touch_sense", 4096, nullptr, 5, &s_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool present() { return s_present; }

}  // namespace touch

#else  // Waveshare: the buttons are on the TCA9555 expander, no touch pad

namespace touch {
esp_err_t start() { return ESP_ERR_NOT_SUPPORTED; }
bool present() { return false; }
}  // namespace touch

#endif
