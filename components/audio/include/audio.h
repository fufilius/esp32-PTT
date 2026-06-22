#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t audio_init(void);
esp_err_t audio_start_input_test(void);
esp_err_t audio_play_test_tone(void);
esp_err_t audio_record_and_playback_test(uint32_t duration_ms);
esp_err_t audio_record_samples(int16_t *samples, size_t sample_count, int32_t *peak);
esp_err_t audio_play_samples(const int16_t *samples, size_t sample_count);
esp_err_t audio_mic_start(void);
esp_err_t audio_mic_read(int16_t *samples, size_t sample_capacity, size_t *samples_read, int32_t *peak);
esp_err_t audio_mic_stop(void);
esp_err_t audio_speaker_start(void);
esp_err_t audio_speaker_write(const int16_t *samples, size_t sample_count);
esp_err_t audio_speaker_stop(void);
