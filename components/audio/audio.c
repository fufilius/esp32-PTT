#include "audio.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_mic_rx_chan;
static i2s_chan_handle_t s_spk_tx_chan;
static bool s_audio_busy;

#define MIC_READ_SAMPLES 256
#define SPEAKER_TONE_HZ 880
#define SPEAKER_TONE_MS 600
#define SPEAKER_AMPLITUDE 9000
#define AUDIO_PI 3.14159265358979323846f
#define RECORD_PLAYBACK_MS 3000
#define MIC_TO_SPEAKER_SHIFT 12

static int16_t mic_sample_to_speaker_sample(int32_t mic_sample)
{
    int32_t sample = mic_sample >> MIC_TO_SPEAKER_SHIFT;
    if (sample > INT16_MAX) {
        sample = INT16_MAX;
    } else if (sample < INT16_MIN) {
        sample = INT16_MIN;
    }

    return (int16_t)sample;
}

static esp_err_t speaker_write_samples(const int16_t *samples, size_t sample_count)
{
    const uint8_t *data = (const uint8_t *)samples;
    size_t bytes_remaining = sample_count * sizeof(samples[0]);

    while (bytes_remaining > 0) {
        size_t bytes_written = 0;
        ESP_RETURN_ON_ERROR(
            i2s_channel_write(s_spk_tx_chan, data, bytes_remaining, &bytes_written, pdMS_TO_TICKS(1000)),
            TAG,
            "speaker write failed");
        data += bytes_written;
        bytes_remaining -= bytes_written;
    }

    return ESP_OK;
}

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG,
             "I2S mic: BCLK=%d WS=%d DIN=%d; speaker: BCLK=%d WS=%d DOUT=%d; sample_rate=%d",
             CONFIG_IPPHONE_I2S_MIC_BCLK,
             CONFIG_IPPHONE_I2S_MIC_WS,
             CONFIG_IPPHONE_I2S_MIC_DIN,
             CONFIG_IPPHONE_I2S_SPK_BCLK,
             CONFIG_IPPHONE_I2S_SPK_WS,
             CONFIG_IPPHONE_I2S_SPK_DOUT,
             CONFIG_IPPHONE_I2S_SAMPLE_RATE);

    i2s_chan_config_t mic_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&mic_chan_cfg, NULL, &s_mic_rx_chan), TAG, "create mic rx channel failed");

    i2s_std_config_t mic_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_IPPHONE_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_IPPHONE_I2S_MIC_BCLK,
            .ws = CONFIG_IPPHONE_I2S_MIC_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = CONFIG_IPPHONE_I2S_MIC_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    mic_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_mic_rx_chan, &mic_std_cfg), TAG, "init mic rx failed");

    i2s_chan_config_t spk_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&spk_chan_cfg, &s_spk_tx_chan, NULL), TAG, "create speaker tx channel failed");

    i2s_std_config_t spk_std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_IPPHONE_I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_IPPHONE_I2S_SPK_BCLK,
            .ws = CONFIG_IPPHONE_I2S_SPK_WS,
            .dout = CONFIG_IPPHONE_I2S_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    spk_std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_spk_tx_chan, &spk_std_cfg), TAG, "init speaker tx failed");

    ESP_LOGI(TAG, "audio test ready");
    return ESP_OK;
}

esp_err_t audio_start_input_test(void)
{
    ESP_LOGI(TAG, "input level test is disabled; press BOOT to record and play voice");
    return ESP_OK;
}

esp_err_t audio_play_test_tone(void)
{
    ESP_RETURN_ON_FALSE(s_spk_tx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker tx channel is not initialized");

    ESP_LOGI(TAG, "playing %d Hz test tone for %d ms", SPEAKER_TONE_HZ, SPEAKER_TONE_MS);
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_spk_tx_chan), TAG, "enable speaker tx failed");

    const int sample_rate = CONFIG_IPPHONE_I2S_SAMPLE_RATE;
    const int total_samples = (sample_rate * SPEAKER_TONE_MS) / 1000;
    int16_t samples[128] = {0};
    int samples_written = 0;

    while (samples_written < total_samples) {
        const int max_batch = (int)(sizeof(samples) / sizeof(samples[0]));
        const int remaining = total_samples - samples_written;
        const int batch = remaining < max_batch ? remaining : max_batch;
        for (int i = 0; i < batch; ++i) {
            const float phase = 2.0f * AUDIO_PI * (float)SPEAKER_TONE_HZ * (float)(samples_written + i) / (float)sample_rate;
            samples[i] = (int16_t)(sinf(phase) * SPEAKER_AMPLITUDE);
        }

        size_t bytes_written = 0;
        ESP_RETURN_ON_ERROR(
            i2s_channel_write(s_spk_tx_chan, samples, batch * sizeof(samples[0]), &bytes_written, pdMS_TO_TICKS(1000)),
            TAG,
            "write speaker tone failed");
        samples_written += bytes_written / sizeof(samples[0]);
    }

    i2s_channel_disable(s_spk_tx_chan);
    return ESP_OK;
}

esp_err_t audio_record_and_playback_test(uint32_t duration_ms)
{
    ESP_RETURN_ON_FALSE(s_mic_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "mic rx channel is not initialized");
    ESP_RETURN_ON_FALSE(s_spk_tx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker tx channel is not initialized");
    ESP_RETURN_ON_FALSE(!s_audio_busy, ESP_ERR_INVALID_STATE, TAG, "audio is busy");

    if (duration_ms == 0) {
        duration_ms = RECORD_PLAYBACK_MS;
    }

    const size_t sample_count = ((size_t)CONFIG_IPPHONE_I2S_SAMPLE_RATE * duration_ms) / 1000;
    int16_t *recorded = (int16_t *)calloc(sample_count, sizeof(recorded[0]));
    if (recorded == NULL) {
        free(recorded);
        ESP_LOGE(TAG, "record buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "recording voice for %lu ms (%u samples)", (unsigned long)duration_ms, (unsigned)sample_count);
    int32_t peak = 0;
    esp_err_t err = audio_record_samples(recorded, sample_count, &peak);
    if (err != ESP_OK) {
        free(recorded);
        ESP_RETURN_ON_ERROR(err, TAG, "record/playback failed");
    }

    ESP_LOGI(TAG, "record complete, peak=%ld; playing back", peak);
    err = audio_play_samples(recorded, sample_count);
    free(recorded);

    ESP_RETURN_ON_ERROR(err, TAG, "record/playback failed");
    ESP_LOGI(TAG, "record/playback test complete");
    return ESP_OK;
}

esp_err_t audio_record_samples(int16_t *samples, size_t sample_count, int32_t *peak)
{
    ESP_RETURN_ON_FALSE(s_mic_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "mic rx channel is not initialized");
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "record samples buffer is null");

    int32_t local_peak = 0;
    esp_err_t err = audio_mic_start();
    ESP_RETURN_ON_ERROR(err, TAG, "start mic failed");

    size_t samples_recorded = 0;
    while (samples_recorded < sample_count) {
        size_t samples_read = 0;
        int32_t chunk_peak = 0;
        err = audio_mic_read(&samples[samples_recorded], sample_count - samples_recorded, &samples_read, &chunk_peak);
        if (err != ESP_OK) {
            break;
        }

        samples_recorded += samples_read;
        if (chunk_peak > local_peak) {
            local_peak = chunk_peak;
        }
    }

    esp_err_t disable_err = audio_mic_stop();
    if (err == ESP_OK) {
        err = disable_err;
    }

    if (peak != NULL) {
        *peak = local_peak;
    }
    return err;
}

esp_err_t audio_play_samples(const int16_t *samples, size_t sample_count)
{
    ESP_RETURN_ON_FALSE(s_spk_tx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker tx channel is not initialized");
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "play samples buffer is null");

    esp_err_t err = audio_speaker_start();
    ESP_RETURN_ON_ERROR(err, TAG, "start speaker failed");

    err = audio_speaker_write(samples, sample_count);
    esp_err_t disable_err = audio_speaker_stop();
    if (err == ESP_OK) {
        err = disable_err;
    }

    return err;
}

esp_err_t audio_mic_start(void)
{
    ESP_RETURN_ON_FALSE(s_mic_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "mic rx channel is not initialized");
    ESP_RETURN_ON_FALSE(!s_audio_busy, ESP_ERR_INVALID_STATE, TAG, "audio is busy");

    s_audio_busy = true;
    esp_err_t err = i2s_channel_enable(s_mic_rx_chan);
    if (err != ESP_OK) {
        s_audio_busy = false;
        ESP_LOGE(TAG, "enable mic rx failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t audio_mic_read(int16_t *samples, size_t sample_capacity, size_t *samples_read, int32_t *peak)
{
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "mic samples buffer is null");
    ESP_RETURN_ON_FALSE(sample_capacity > 0, ESP_ERR_INVALID_ARG, TAG, "mic samples capacity is zero");
    ESP_RETURN_ON_FALSE(s_audio_busy, ESP_ERR_INVALID_STATE, TAG, "mic is not started");

    int32_t mic_chunk[MIC_READ_SAMPLES] = {0};
    const size_t samples_to_read = sample_capacity < MIC_READ_SAMPLES ? sample_capacity : MIC_READ_SAMPLES;

    size_t bytes_read = 0;
    esp_err_t err =
        i2s_channel_read(s_mic_rx_chan, mic_chunk, samples_to_read * sizeof(mic_chunk[0]), &bytes_read, pdMS_TO_TICKS(1000));
    ESP_RETURN_ON_ERROR(err, TAG, "mic read failed");

    const size_t read_count = bytes_read / sizeof(mic_chunk[0]);
    int32_t local_peak = 0;
    for (size_t i = 0; i < read_count; ++i) {
        const int16_t sample = mic_sample_to_speaker_sample(mic_chunk[i]);
        const int32_t abs_sample = abs(sample);
        if (abs_sample > local_peak) {
            local_peak = abs_sample;
        }
        samples[i] = sample;
    }

    if (samples_read != NULL) {
        *samples_read = read_count;
    }
    if (peak != NULL) {
        *peak = local_peak;
    }
    return ESP_OK;
}

esp_err_t audio_mic_stop(void)
{
    ESP_RETURN_ON_FALSE(s_mic_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "mic rx channel is not initialized");
    ESP_RETURN_ON_FALSE(s_audio_busy, ESP_ERR_INVALID_STATE, TAG, "mic is not started");

    esp_err_t err = i2s_channel_disable(s_mic_rx_chan);
    s_audio_busy = false;
    return err;
}

esp_err_t audio_speaker_start(void)
{
    ESP_RETURN_ON_FALSE(s_spk_tx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker tx channel is not initialized");
    esp_err_t err = i2s_channel_enable(s_spk_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "enable speaker tx failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t audio_speaker_write(const int16_t *samples, size_t sample_count)
{
    ESP_RETURN_ON_FALSE(samples != NULL, ESP_ERR_INVALID_ARG, TAG, "speaker samples buffer is null");
    return speaker_write_samples(samples, sample_count);
}

esp_err_t audio_speaker_stop(void)
{
    ESP_RETURN_ON_FALSE(s_spk_tx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "speaker tx channel is not initialized");
    return i2s_channel_disable(s_spk_tx_chan);
}
