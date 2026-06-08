#include "keypad.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/task.h"

static const char *TAG = "keypad";

static const gpio_num_t ROW_PINS[4] = {
    CONFIG_IPPHONE_KEYPAD_ROW0,
    CONFIG_IPPHONE_KEYPAD_ROW1,
    CONFIG_IPPHONE_KEYPAD_ROW2,
    CONFIG_IPPHONE_KEYPAD_ROW3,
};

static const gpio_num_t COL_PINS[4] = {
    CONFIG_IPPHONE_KEYPAD_COL0,
    CONFIG_IPPHONE_KEYPAD_COL1,
    CONFIG_IPPHONE_KEYPAD_COL2,
    CONFIG_IPPHONE_KEYPAD_COL3,
};

static const char KEYMAP[4][4] = {
    {'1', '2', '3', 'A'},
    {'4', '5', '6', 'B'},
    {'7', '8', '9', 'C'},
    {'*', '0', '#', 'D'},
};

esp_err_t keypad_init(void)
{
    for (size_t row = 0; row < 4; ++row) {
        gpio_config_t row_cfg = {
            .pin_bit_mask = 1ULL << ROW_PINS[row],
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&row_cfg), TAG, "row gpio config failed");
        ESP_RETURN_ON_ERROR(gpio_set_level(ROW_PINS[row], 1), TAG, "row gpio level failed");
    }

    for (size_t col = 0; col < 4; ++col) {
        gpio_config_t col_cfg = {
            .pin_bit_mask = 1ULL << COL_PINS[col],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&col_cfg), TAG, "column gpio config failed");
    }

    ESP_LOGI(TAG, "keypad ready");
    return ESP_OK;
}

esp_err_t keypad_read_key(char *key, TickType_t timeout_ticks)
{
    ESP_RETURN_ON_FALSE(key != NULL, ESP_ERR_INVALID_ARG, TAG, "key is NULL");

    const TickType_t start = xTaskGetTickCount();
    *key = 0;

    do {
        for (size_t row = 0; row < 4; ++row) {
            for (size_t i = 0; i < 4; ++i) {
                gpio_set_level(ROW_PINS[i], 1);
            }
            gpio_set_level(ROW_PINS[row], 0);
            esp_rom_delay_us(30);

            for (size_t col = 0; col < 4; ++col) {
                if (gpio_get_level(COL_PINS[col]) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(25));
                    if (gpio_get_level(COL_PINS[col]) == 0) {
                        *key = KEYMAP[row][col];
                        while (gpio_get_level(COL_PINS[col]) == 0) {
                            vTaskDelay(pdMS_TO_TICKS(10));
                        }
                        return ESP_OK;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    } while ((xTaskGetTickCount() - start) < timeout_ticks);

    return ESP_ERR_TIMEOUT;
}
