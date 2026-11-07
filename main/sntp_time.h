#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool is_synced;
    int hour12;
    int min;
    bool is_pm;
    char time_str[16];
    char am_pm[4];
    char date_str[32];
} clock_display_time_t;

typedef void (*sntp_sync_cb_t)(void);

esp_err_t sntp_time_init(sntp_sync_cb_t cb);
void sntp_time_update_tz(const char *tz_string);
void sntp_time_restart(void);
bool sntp_time_is_synced(void);
bool sntp_time_get_local(struct tm *out_tm);
void sntp_time_get_current(clock_display_time_t *out_time);

#ifdef __cplusplus
}
#endif
