#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t console_sim_init(void);

// Called periodically (every 30ms) from GUI/main loop
void console_sim_tick(void);

// Inject system event logs into console (e.g. Wi-Fi connected, Touch event)
void console_sim_notify_wifi(bool connected, const char *ip_str);
void console_sim_notify_touch(int x, int y);

#ifdef __cplusplus
}
#endif
