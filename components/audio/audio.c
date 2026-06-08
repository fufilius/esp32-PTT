#include "audio.h"

#include "esp_log.h"

static const char *TAG = "audio";

esp_err_t audio_init(void)
{
    ESP_LOGI(TAG,
             "I2S defaults: BCLK=%d WS=%d DIN=%d DOUT=%d sample_rate=%d",
             CONFIG_IPPHONE_I2S_BCLK,
             CONFIG_IPPHONE_I2S_WS,
             CONFIG_IPPHONE_I2S_DIN,
             CONFIG_IPPHONE_I2S_DOUT,
             CONFIG_IPPHONE_I2S_SAMPLE_RATE);
    ESP_LOGI(TAG, "audio component prepared; enable INMP441/MAX98357A logic in stage 2/3");
    return ESP_OK;
}

esp_err_t audio_start_input_test(void)
{
    ESP_LOGI(TAG, "input test placeholder");
    return ESP_OK;
}

esp_err_t audio_play_test_tone(void)
{
    ESP_LOGI(TAG, "tone playback placeholder");
    return ESP_OK;
}
