#pragma once

#include "esp_err.h"

esp_err_t audio_init(void);
esp_err_t audio_start_input_test(void);
esp_err_t audio_play_test_tone(void);
