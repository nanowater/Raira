// The EC11 encoder: the PCNT peripheral decodes the two quadrature signals in hardware (no interrupt per edge), a small
// task reads the count every 10 ms and polls the push button. Nothing here touches LVGL: the poller calls are
// thread-safe, and the one app call (`music_app_encoder_seen`) is posted to the LVGL thread.
#include "encoder.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "media_poller.h"
#include "music_app.h"

static const char * TAG = "encoder";

static const gpio_num_t PIN_A   = GPIO_NUM_31;
static const gpio_num_t PIN_B   = GPIO_NUM_30;
static const gpio_num_t PIN_KEY = GPIO_NUM_33;

// An EC11 gives 4 counts (every edge of both signals) per detent. Some parts give 2: change this if one click of the
// knob moves the volume by two steps, or needs two clicks for one step.
static const int COUNTS_PER_DETENT = 4;
// Swap to turn the knob the other way round (or swap the A and B wires).
static const bool REVERSE = false;

static const int POLL_MS        = 10;
static const int DEBOUNCE_POLLS = 3;   // the key must read the same this many polls in a row (30 ms)
static const int LONG_PRESS_MS  = 600; // longer than this is not a click (reserved for later)

static pcnt_unit_handle_t g_unit;
static bool g_seen = false;

// The first input: tell the app once (on the LVGL thread).
static void note_input()
{
    if(g_seen) return;
    g_seen = true;
    media_post_to_ui([] { music_app_encoder_seen(); });
}

static void encoder_task(void *)
{
    int rest       = 0;     // counts that do not make a whole detent yet
    bool key_down  = false; // debounced state
    int same       = 0;     // polls the raw key state has been the same
    bool last_raw  = false;
    int down_polls = 0;

    while(true) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        // Turning: read the count and start again from zero, so nothing is lost between two reads.
        int count = 0;
        pcnt_unit_get_count(g_unit, &count);
        if(count != 0) {
            pcnt_unit_clear_count(g_unit);
            rest += REVERSE ? -count : count;
            const int steps = rest / COUNTS_PER_DETENT;
            if(steps != 0) {
                rest -= steps * COUNTS_PER_DETENT;
                note_input();
                media_poller_volume_step(steps);
            }
        }

        // The key pulls the line to GND when pressed. A click is counted on release.
        const bool raw = gpio_get_level(PIN_KEY) == 0;
        same           = raw == last_raw ? same + 1 : 1;
        last_raw       = raw;
        if(same < DEBOUNCE_POLLS || raw == key_down) {
            if(key_down) down_polls++;
            continue;
        }
        key_down = raw;
        if(key_down) {
            down_polls = 0;
        } else if(down_polls * POLL_MS < LONG_PRESS_MS) {
            note_input();
            media_poller_command(MUSIC_UI_CMD_PLAY_PAUSE);
        }
    }
}

bool encoder_start()
{
    // The key: a plain input. The module has its own pull-up to 3V3; the internal one only matters if it is missing.
    gpio_config_t key = {};
    key.pin_bit_mask  = 1ULL << PIN_KEY;
    key.mode          = GPIO_MODE_INPUT;
    key.pull_up_en    = GPIO_PULLUP_ENABLE;
    if(gpio_config(&key) != ESP_OK) {
        ESP_LOGE(TAG, "key pin setup failed");
        return false;
    }

    // One PCNT unit with two channels counts the quadrature signal: on each edge of one signal the level of the other
    // says which way the knob turns (the standard decoding from the ESP-IDF rotary_encoder example).
    pcnt_unit_config_t unit_cfg = {};
    unit_cfg.low_limit          = -1000;
    unit_cfg.high_limit         = 1000;
    if(pcnt_new_unit(&unit_cfg, &g_unit) != ESP_OK) {
        ESP_LOGE(TAG, "pcnt unit failed");
        return false;
    }

    // Ignore pulses shorter than 1 us (electrical glitches); the contact bounce of a few ms is handled by the
    // up/down counting itself, which cancels out.
    pcnt_glitch_filter_config_t filter = {};
    filter.max_glitch_ns               = 1000;
    pcnt_unit_set_glitch_filter(g_unit, &filter);

    pcnt_chan_config_t chan_a_cfg = {};
    chan_a_cfg.edge_gpio_num      = PIN_A;
    chan_a_cfg.level_gpio_num     = PIN_B;
    pcnt_chan_config_t chan_b_cfg = {};
    chan_b_cfg.edge_gpio_num      = PIN_B;
    chan_b_cfg.level_gpio_num     = PIN_A;
    pcnt_channel_handle_t chan_a  = nullptr;
    pcnt_channel_handle_t chan_b  = nullptr;
    if(pcnt_new_channel(g_unit, &chan_a_cfg, &chan_a) != ESP_OK ||
       pcnt_new_channel(g_unit, &chan_b_cfg, &chan_b) != ESP_OK) {
        ESP_LOGE(TAG, "pcnt channel failed");
        return false;
    }
    pcnt_channel_set_edge_action(chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);

    // PCNT sets the two pins up as inputs; the pull-ups are added afterwards.
    gpio_set_pull_mode(PIN_A, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(PIN_B, GPIO_PULLUP_ONLY);

    if(pcnt_unit_enable(g_unit) != ESP_OK || pcnt_unit_clear_count(g_unit) != ESP_OK ||
       pcnt_unit_start(g_unit) != ESP_OK) {
        ESP_LOGE(TAG, "pcnt start failed");
        return false;
    }

    // Core 0, below the LVGL task: the knob must never make the screen stutter.
    if(xTaskCreatePinnedToCore(encoder_task, "encoder", 3072, nullptr, 3, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "task failed");
        return false;
    }
    ESP_LOGI(TAG, "encoder ready: A=%d B=%d KEY=%d", (int)PIN_A, (int)PIN_B, (int)PIN_KEY);
    return true;
}
