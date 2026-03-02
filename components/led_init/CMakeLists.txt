idf_component_register(
    SRCS "led_init.c" "status_rgb_ws2812.c"
    INCLUDE_DIRS "."
    REQUIRES driver led_strip
)