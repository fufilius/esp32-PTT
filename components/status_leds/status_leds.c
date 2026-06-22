#include "status_leds.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "status_leds";

static bool s_tx_active;
static bool s_rx_active;

static uint32_t led_level(bool on, int active_level)
{
    const bool active_high = active_level != 0;
    return on == active_high ? 1U : 0U;
}

static void status_leds_apply(void)
{
    const bool voice_active = s_tx_active || s_rx_active;

    gpio_set_level(CONFIG_IPPHONE_LED_GREEN_GPIO, led_level(!voice_active, CONFIG_IPPHONE_LED_GREEN_ACTIVE_LEVEL));
    gpio_set_level(CONFIG_IPPHONE_LED_RED_GPIO, led_level(voice_active, CONFIG_IPPHONE_LED_RED_ACTIVE_LEVEL));
}

esp_err_t status_leds_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CONFIG_IPPHONE_LED_GREEN_GPIO) | (1ULL << CONFIG_IPPHONE_LED_RED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "configure led gpios failed");
    s_tx_active = false;
    s_rx_active = false;
    status_leds_apply();

    ESP_LOGI(TAG,
             "status LEDs ready: green GPIO%d active=%d, red GPIO%d active=%d",
             CONFIG_IPPHONE_LED_GREEN_GPIO,
             CONFIG_IPPHONE_LED_GREEN_ACTIVE_LEVEL,
             CONFIG_IPPHONE_LED_RED_GPIO,
             CONFIG_IPPHONE_LED_RED_ACTIVE_LEVEL);

    return ESP_OK;
}

void status_leds_set_tx(bool active)
{
    s_tx_active = active;
    status_leds_apply();
}

void status_leds_set_rx(bool active)
{
    s_rx_active = active;
    status_leds_apply();
}
