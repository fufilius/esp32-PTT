idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES audio button display network_audio status_leds storage
)
