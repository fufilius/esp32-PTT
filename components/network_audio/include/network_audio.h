#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t network_audio_init(void);
esp_err_t network_audio_send_recording(uint32_t duration_ms);
esp_err_t network_audio_stream_start(void);
esp_err_t network_audio_stream_send_next(bool end_after_packet);
esp_err_t network_audio_stream_stop(void);
