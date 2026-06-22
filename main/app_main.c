#include "audio.h"
#include "display.h"
#include "esp_log.h"
#include "keypad.h"
#include "network_audio.h"

static const char *TAG = "ipphone";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32 IPphone стартует");

    ESP_ERROR_CHECK(keypad_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(network_audio_init());

    ESP_ERROR_CHECK(audio_play_test_tone());
    ESP_ERROR_CHECK(audio_start_input_test());

    display_set_status("Готов");
    ESP_LOGI(TAG, "Проверка: хлопните рядом с микрофоном и смотрите mic level в мониторе");
    ESP_LOGI(TAG, "Этап 1: нажмите кнопку на клавиатуре 4x4");

    while (true) {
        char key = 0;
        esp_err_t err = keypad_read_key(&key, pdMS_TO_TICKS(50));
        if (err == ESP_OK && key != 0) {
            ESP_LOGI(TAG, "Нажата кнопка: %c", key);
            display_set_dialed_key(key);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
