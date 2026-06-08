idf_component_register(
    SRCS "network_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event nvs_flash
)
