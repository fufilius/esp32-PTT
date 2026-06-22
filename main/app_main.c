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

    display_set_status("Готов");
    ESP_LOGI(TAG, "Нажмите любую кнопку: ESP32 запишет 3 секунды голоса и проиграет запись");

    while (true) {
        char key = 0;
        esp_err_t err = keypad_read_key(&key, pdMS_TO_TICKS(50));
        if (err == ESP_OK && key != 0) {
            ESP_LOGI(TAG, "Нажата кнопка: %c", key);
            display_set_dialed_key(key);
            display_set_status("Запись");
            esp_err_t audio_err = audio_record_and_playback_test(3000);
            if (audio_err != ESP_OK) {
                ESP_LOGE(TAG, "Ошибка аудиотеста: %s", esp_err_to_name(audio_err));
            }
            display_set_status("Готов");
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
