#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LED_RED = 0,
    LED_GRN,
    LED_BLU,
} led_t;

void led_init(void);
void led_set(led_t led, bool on);
void led_all_off(void);

void status_rgb_set(uint8_t r, uint8_t g, uint8_t b);
void status_rgb_off(void);