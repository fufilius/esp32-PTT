#pragma once

#include "esp_err.h"

esp_err_t display_init(void);
esp_err_t display_set_status(const char *status);
esp_err_t display_set_dialed_key(char key);
