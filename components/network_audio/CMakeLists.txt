idf_component_register(
    SRCS "network_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES audio status_leds esp_wifi esp_event esp_netif nvs_flash
)
