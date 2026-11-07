#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[65];
    char timezone[32];              // e.g. "JST-9"
    char weather_location[48];      // e.g. "Tokyo", "Osaka", or "35.68,139.76"
    uint8_t brightness;             // Backlight percent (0, 25, 50, 100)
    char oauth_client_id[128];      // Google OAuth Client ID
    char oauth_client_secret[128];  // Google OAuth Client Secret
    char oauth_refresh_token[512];  // Google OAuth Refresh Token (Antigravity)
} app_settings_t;

extern app_settings_t g_settings;

esp_err_t settings_mgr_init(void);
const app_settings_t *settings_mgr_get(void);
esp_err_t settings_mgr_save(const app_settings_t *settings);
esp_err_t settings_mgr_load(app_settings_t *settings);
esp_err_t settings_mgr_set_brightness(uint8_t brightness);

#ifdef __cplusplus
}
#endif
