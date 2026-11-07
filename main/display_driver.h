#pragma once

#include "esp_err.h"
#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DRIVER_UNKNOWN = 0,
    DRIVER_ILI9341,
    DRIVER_ST7789
} lcd_driver_type_t;

esp_err_t display_driver_init(void);
void display_driver_set_backlight(uint8_t percent);
uint8_t display_driver_get_brightness(void);
uint8_t display_driver_cycle_brightness(void);
lcd_driver_type_t display_driver_get_type(void);

#ifdef __cplusplus
}
#endif
