#include "sdkconfig.h"

#if CONFIG_LED_STATUS_WS2812

#include "esp_err.h"
#include "led_strip.h"

static led_strip_handle_t s_strip;

void status_rgb_ws2812_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_LED_STATUS_WS2812_GPIO,
        .max_leds = CONFIG_LED_STATUS_WS2812_NUM_LEDS,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz como el ejemplo
        .flags.with_dma = false,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);
}

void status_rgb_ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) return;
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

#endif