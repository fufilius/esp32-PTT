#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t storage_init(void);
esp_err_t storage_record_start(void);
esp_err_t storage_record_next(void);
esp_err_t storage_record_stop(void);
esp_err_t storage_play_recording(void);
bool storage_record_is_active(void);
