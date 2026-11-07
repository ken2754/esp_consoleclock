#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*touch_callback_t)(void);

esp_err_t touch_driver_init(touch_callback_t on_touch_cb);
void touch_driver_poll(void);
bool touch_driver_is_pressed(void);
bool touch_driver_get_xy(int *x, int *y);

#ifdef __cplusplus
}
#endif
