#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char version[16];
    bool version_valid;
    uint8_t led_effect;
    bool led_effect_valid;
    uint8_t led_brightness;
    bool led_brightness_valid;
    uint8_t led_speed;
    bool led_speed_valid;
    uint32_t led_color;
    bool led_color_valid;
} xvf3800_status_t;

esp_err_t xvf3800_init(void);
esp_err_t xvf3800_get_status(xvf3800_status_t *status);
esp_err_t xvf3800_set_parameter(const char *name, uint32_t value);
