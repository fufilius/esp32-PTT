#include "audio.h"

#include "esp_log.h"

static const char *TAG = "audio";

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
