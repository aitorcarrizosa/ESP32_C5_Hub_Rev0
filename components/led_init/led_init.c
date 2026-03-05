#include "led_init.h"

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_err.h"

#if CONFIG_LED_STATUS_HUB_GPIO
#define LED_RED_GPIO ((int)CONFIG_LED_RED_GPIO)
#define LED_GRN_GPIO ((int)CONFIG_LED_GRN_GPIO)
#define LED_BLU_GPIO ((int)CONFIG_LED_BLU_GPIO)
#else
#define LED_RED_GPIO (-1)
#define LED_GRN_GPIO (-1)
#define LED_BLU_GPIO (-1)
#endif

static void gpio_out_init_if_valid(int gpio_num)
{
    if (gpio_num < 0) return;

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << gpio_num),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(gpio_num, 0);
}

static int led_to_gpio(led_t led)
{
    switch (led) {
        case LED_RED: return LED_RED_GPIO;
        case LED_GRN: return LED_GRN_GPIO;
        case LED_BLU: return LED_BLU_GPIO;
        default:      return -1;
    }
}

void led_init(void)
{
    gpio_out_init_if_valid(LED_RED_GPIO);
    gpio_out_init_if_valid(LED_GRN_GPIO);
    gpio_out_init_if_valid(LED_BLU_GPIO);

#if CONFIG_LED_STATUS_WS2812
    extern void status_rgb_ws2812_init(void);
    status_rgb_ws2812_init();
#endif
}

void led_set(led_t led, bool on)
{
    int gpio_num = led_to_gpio(led);
    if (gpio_num < 0) return;

    gpio_set_level(gpio_num, on ? 1 : 0);       // Hub: active-HIGH (MOSFET low-side)
}

void led_all_off(void)
{
    if (LED_RED_GPIO >= 0) gpio_set_level(LED_RED_GPIO, 0);
    if (LED_GRN_GPIO >= 0) gpio_set_level(LED_GRN_GPIO, 0);
    if (LED_BLU_GPIO >= 0) gpio_set_level(LED_BLU_GPIO, 0);
}

void status_rgb_set(uint8_t r, uint8_t g, uint8_t b)
{
#if CONFIG_LED_STATUS_WS2812
    extern void status_rgb_ws2812_set(uint8_t r, uint8_t g, uint8_t b);
    status_rgb_ws2812_set(r, g, b);

#elif CONFIG_LED_STATUS_HUB_GPIO
    const uint8_t th = 8;    // Discrete LEDs -> threshold mapping (active-high)
    led_set(LED_RED, (r >= th));
    led_set(LED_GRN, (g >= th));
    led_set(LED_BLU, (b >= th));

#else
    (void)r; (void)g; (void)b; // LED_STATUS_NONE
#endif
}

void status_rgb_off(void)
{
    status_rgb_set(0, 0, 0);
}