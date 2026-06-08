#include "display.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "display";

esp_err_t display_init(void)
{
    ESP_LOGI(TAG,
             "display placeholder ready; I2C SDA=%d SCL=%d",
             CONFIG_IPPHONE_I2C_SDA,
             CONFIG_IPPHONE_I2C_SCL);
    return ESP_OK;
}

esp_err_t display_set_status(const char *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is NULL");
    ESP_LOGI(TAG, "status: %s", status);
    return ESP_OK;
}

esp_err_t display_set_dialed_key(char key)
{
    ESP_LOGI(TAG, "dialed key: %c", key);
    return ESP_OK;
}
