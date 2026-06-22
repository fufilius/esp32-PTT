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
static bool s_mic_test_started;

#define MIC_READ_SAMPLES 256
#define SPEAKER_TONE_HZ 880
#define SPEAKER_TONE_MS 600
#define SPEAKER_AMPLITUDE 9000
#define AUDIO_PI 3.14159265358979323846f

static void mic_level_task(void *arg)
{
    int32_t samples[MIC_READ_SAMPLES] = {0};
    ESP_ERROR_CHECK(i2s_channel_enable(s_mic_rx_chan));

    while (true) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(
            s_mic_rx_chan,
            samples,
            sizeof(samples),
            &bytes_read,
            pdMS_TO_TICKS(1000));

        if (err != ESP_OK || bytes_read == 0) {
            ESP_LOGW(TAG, "mic read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        const size_t sample_count = bytes_read / sizeof(samples[0]);
        int64_t sum_abs = 0;
        int32_t peak = 0;

        for (size_t i = 0; i < sample_count; ++i) {
            int32_t sample = samples[i] >> 14;
            int32_t abs_sample = abs(sample);
            sum_abs += abs_sample;
            if (abs_sample > peak) {
                peak = abs_sample;
            }
        }

        ESP_LOGI(TAG,
                 "mic level: avg=%lld peak=%ld samples=%u",
                 sum_abs / sample_count,
                 peak,
                 (unsigned)sample_count);

        vTaskDelay(pdMS_TO_TICKS(500));
    }
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
    ESP_RETURN_ON_FALSE(s_mic_rx_chan != NULL, ESP_ERR_INVALID_STATE, TAG, "mic rx channel is not initialized");
    if (!s_mic_test_started) {
        BaseType_t ok = xTaskCreate(mic_level_task, "mic_level", 4096, NULL, 5, NULL);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "create mic task failed");
        s_mic_test_started = true;
    }
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
