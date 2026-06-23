#include <stdbool.h>

#include "audio.h"
#include "button.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_audio.h"
#include "status_leds.h"
#include "storage.h"

static const char *TAG = "ipphone";
static bool s_sd_ready;

typedef enum {
    STATE_IDLE,
    STATE_TX,
    STATE_SD_RECORD,
    STATE_SD_PLAYBACK,
    STATE_ERROR,
} app_state_t;

static app_state_t s_state = STATE_IDLE;

static const char *state_name(app_state_t state)
{
    switch (state) {
    case STATE_IDLE:
        return "IDLE";
    case STATE_TX:
        return "TX";
    case STATE_SD_RECORD:
        return "SD_RECORD";
    case STATE_SD_PLAYBACK:
        return "SD_PLAYBACK";
    case STATE_ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

static void set_state(app_state_t state)
{
    if (s_state == state) {
        return;
    }

    ESP_LOGI(TAG, "FSM: %s -> %s", state_name(s_state), state_name(state));
    s_state = state;

    switch (s_state) {
    case STATE_IDLE:
        display_set_status("Ready");
        break;
    case STATE_TX:
        display_set_status("LIVE");
        break;
    case STATE_SD_RECORD:
        display_set_status("REC");
        break;
    case STATE_SD_PLAYBACK:
        display_set_status("PLAY");
        break;
    case STATE_ERROR:
        display_set_status("ERROR");
        break;
    default:
        break;
    }
}

static void run_live_push_to_talk(void)
{
    ESP_LOGI(TAG, "live ESP-NOW PTT started");

    esp_err_t err = network_audio_stream_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "live PTT start failed: %s", esp_err_to_name(err));
        set_state(STATE_ERROR);
        return;
    }

    while (!button_release_confirmed()) {
        err = network_audio_stream_send_next(false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "live PTT packet failed: %s", esp_err_to_name(err));
            break;
        }
    }

    esp_err_t stop_err = network_audio_stream_stop();
    if (stop_err != ESP_OK) {
        ESP_LOGE(TAG, "live PTT stop failed: %s", esp_err_to_name(stop_err));
    }

    ESP_LOGI(TAG, "live ESP-NOW PTT stopped");
}

static void start_sd_recording(void)
{
    if (!s_sd_ready) {
        ESP_LOGW(TAG, "microSD is not ready");
        set_state(STATE_ERROR);
        return;
    }

    ESP_LOGI(TAG, "starting microSD recording");
    status_leds_set_tx(true);

    esp_err_t err = storage_record_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start microSD recording failed: %s", esp_err_to_name(err));
        status_leds_set_tx(false);
        set_state(STATE_ERROR);
        return;
    }
}

static void stop_sd_recording_and_play(void)
{
    ESP_LOGI(TAG, "stopping microSD recording and playing it back");
    esp_err_t err = storage_record_stop();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stop microSD recording failed: %s", esp_err_to_name(err));
        status_leds_set_tx(false);
        set_state(STATE_ERROR);
        return;
    }

    err = storage_play_recording();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "play microSD recording failed: %s", esp_err_to_name(err));
        set_state(STATE_ERROR);
        status_leds_set_tx(false);
        return;
    }

    status_leds_set_tx(false);
    set_state(STATE_IDLE);
}

static void handle_idle_state(void)
{
    button_event_t event = button_read_event(CONFIG_IPPHONE_BUTTON_HOLD_MS);
    if (event == BUTTON_EVENT_HOLD) {
        set_state(STATE_TX);
        return;
    }

    if (event == BUTTON_EVENT_CLICK) {
        set_state(STATE_SD_RECORD);
    }
}

static void handle_sd_record_state(void)
{
    button_event_t event = button_read_event(CONFIG_IPPHONE_BUTTON_HOLD_MS);
    if (event != BUTTON_EVENT_NONE) {
        button_wait_for_release();
        set_state(STATE_SD_PLAYBACK);
        return;
    }

    esp_err_t err = storage_record_next();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "microSD recording failed: %s", esp_err_to_name(err));
        storage_record_stop();
        status_leds_set_tx(false);
        set_state(STATE_ERROR);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 IPphone starting");

    ESP_ERROR_CHECK(button_init());
    ESP_ERROR_CHECK(status_leds_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(network_audio_init());

    esp_err_t storage_err = storage_init();
    if (storage_err == ESP_OK) {
        s_sd_ready = true;
    } else {
        ESP_LOGW(TAG, "microSD disabled: %s", esp_err_to_name(storage_err));
    }

    display_set_status("Ready");
    ESP_LOGI(TAG, "BOOT click: start/stop microSD recording; BOOT hold: live ESP-NOW PTT");

    while (true) {
        switch (s_state) {
        case STATE_IDLE:
            handle_idle_state();
            break;
        case STATE_TX:
            run_live_push_to_talk();
            button_wait_for_release();
            if (s_state == STATE_TX) {
                set_state(STATE_IDLE);
            }
            break;
        case STATE_SD_RECORD:
            if (!storage_record_is_active()) {
                start_sd_recording();
            }
            handle_sd_record_state();
            break;
        case STATE_SD_PLAYBACK:
            stop_sd_recording_and_play();
            break;
        case STATE_ERROR:
            status_leds_set_tx(false);
            status_leds_set_rx(false);
            if (!button_is_pressed()) {
                set_state(STATE_IDLE);
            }
            break;
        default:
            set_state(STATE_ERROR);
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
