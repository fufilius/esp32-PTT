#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t keypad_init(void);
esp_err_t keypad_read_key(char *key, TickType_t timeout_ticks);
