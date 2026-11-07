#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    bool is_loading;
    char location[32];      // Display location name (e.g. "Tokyo")
    char cond_short[12];    // Short condition code (e.g. "CLR", "PT_CLD", "RAIN")
    float temp_c;           // Current temperature (C)
    int humidity;           // Current relative humidity (%)
    float wind_mps;         // Current wind speed (m/s)
    float max_c;            // Today's high (C)
    float min_c;            // Today's low (C)
    int pop_pct;            // Today's max precipitation probability (%)
    char tmrw_cond[12];     // Tomorrow's condition code
    float tmrw_max_c;       // Tomorrow's high (C)
    float tmrw_min_c;       // Tomorrow's low (C)
    int tmrw_pop_pct;       // Tomorrow's precipitation probability (%)
    int64_t last_update_sec;
} weather_data_t;

esp_err_t weather_client_init(void);
void weather_client_request_refresh(void);
const weather_data_t *weather_client_get_latest(void);

#ifdef __cplusplus
}
#endif
