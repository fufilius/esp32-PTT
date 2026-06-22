#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t status_leds_init(void);
void status_leds_set_tx(bool active);
void status_leds_set_rx(bool active);
