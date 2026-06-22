#include <stdbool.h>

#include "audio.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "keypad.h"
#include "network_audio.h"
#include "status_leds.h"

static const char *TAG = "ipphone";

#ifndef CONFIG_IPPHONE_BOOT_BUTTON_GPIO
#define CONFIG_IPPHONE_BOOT_BUTTON_GPIO 0
#endif

#define BOOT_HOLD_TO_STREAM_MS 500

static esp_err_t boot_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_IPPHONE_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&cfg);
}

static bool boot_button_is_pressed(void)
{
    return gpio_get_level(CONFIG_IPPHONE_BOOT_BUTTON_GPIO) == 0;
}

static void run_push_to_talk(const char *source)
{
    ESP_LOGI(TAG, "%s: ESP-NOW voice send", source);
    display_set_status("TX");

    esp_err_t network_err = network_audio_send_recording(3000);
    if (network_err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW voice send failed: %s", esp_err_to_name(network_err));
    }

    display_set_status("Ready");
}

static void run_live_push_to_talk(const char *source)
{
    ESP_LOGI(TAG, "%s: live ESP-NOW PTT started", source);
    display_set_status("LIVE");

    esp_err_t err = network_audio_stream_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "live PTT start failed: %s", esp_err_to_name(err));
        display_set_status("Ready");
        return;
    }

    while (boot_button_is_pressed()) {
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

    ESP_LOGI(TAG, "%s: live ESP-NOW PTT stopped", source);
    display_set_status("Ready");
}

static void handle_boot_button(void)
{
    if (!boot_button_is_pressed()) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(30));
    if (!boot_button_is_pressed()) {
        return;
    }

    const TickType_t press_start = xTaskGetTickCount();
    while (boot_button_is_pressed() && (xTaskGetTickCount() - press_start) < pdMS_TO_TICKS(BOOT_HOLD_TO_STREAM_MS)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (boot_button_is_pressed()) {
        ESP_ERROR_CHECK(audio_play_test_tone());
        run_live_push_to_talk("BOOT hold");
        return;
    }

    ESP_ERROR_CHECK(audio_play_test_tone());
    run_push_to_talk("BOOT click");
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 IPphone starting");

    ESP_ERROR_CHECK(boot_button_init());
    ESP_ERROR_CHECK(status_leds_init());
    ESP_ERROR_CHECK(keypad_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(network_audio_init());

    ESP_ERROR_CHECK(audio_play_test_tone());

    display_set_status("Ready");
    ESP_LOGI(TAG, "BOOT click: recorded ESP-NOW send; BOOT hold: live ESP-NOW PTT");

    while (true) {
        handle_boot_button();

        char key = 0;
        esp_err_t err = keypad_read_key(&key, pdMS_TO_TICKS(50));
        if (err == ESP_OK && key != 0) {
            ESP_LOGI(TAG, "key pressed: %c", key);
            display_set_dialed_key(key);
            run_push_to_talk("keypad");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
