#include "settings_mgr.h"
#include "config.h"
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "settings_mgr";
#define NVS_NAMESPACE "agyclock"

app_settings_t g_settings = {0};

static void set_defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(app_settings_t));
    strncpy(s->timezone, DEFAULT_TIMEZONE, sizeof(s->timezone) - 1);
    strncpy(s->weather_location, DEFAULT_WEATHER_LOC, sizeof(s->weather_location) - 1);
    s->brightness = DEFAULT_BRIGHTNESS;
    strncpy(s->oauth_client_id, DEFAULT_OAUTH_CLIENT_ID, sizeof(s->oauth_client_id) - 1);
    strncpy(s->oauth_client_secret, DEFAULT_OAUTH_CLIENT_SECRET, sizeof(s->oauth_client_secret) - 1);
}

static esp_err_t nvs_read_str_helper(nvs_handle_t handle, const char *key, char *out, size_t max_len)
{
    size_t required_size = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required_size);
    if (err == ESP_OK && required_size <= max_len) {
        return nvs_get_str(handle, key, out, &required_size);
    }
    return err;
}

esp_err_t settings_mgr_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init nvs: %s", esp_err_to_name(err));
        return err;
    }

    set_defaults(&g_settings);
    settings_mgr_load(&g_settings);

    return ESP_OK;
}

const app_settings_t *settings_mgr_get(void)
{
    return &g_settings;
}

esp_err_t settings_mgr_load(app_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved settings found in NVS (using defaults)");
        return ESP_OK;
    }

    nvs_read_str_helper(handle, "ssid", settings->wifi_ssid, sizeof(settings->wifi_ssid));
    nvs_read_str_helper(handle, "pass", settings->wifi_pass, sizeof(settings->wifi_pass));
    nvs_read_str_helper(handle, "tz", settings->timezone, sizeof(settings->timezone));
    nvs_read_str_helper(handle, "wloc", settings->weather_location, sizeof(settings->weather_location));

    uint8_t u8_val = 0;
    if (nvs_get_u8(handle, "bright", &u8_val) == ESP_OK && u8_val <= 100) {
        settings->brightness = u8_val;
    }

    nvs_read_str_helper(handle, "cid", settings->oauth_client_id, sizeof(settings->oauth_client_id));
    nvs_read_str_helper(handle, "sec", settings->oauth_client_secret, sizeof(settings->oauth_client_secret));

    err = nvs_read_str_helper(handle, "refr", settings->oauth_refresh_token, sizeof(settings->oauth_refresh_token));
    if (err != ESP_OK || strlen(settings->oauth_refresh_token) == 0) {
        nvs_read_str_helper(handle, "refresh", settings->oauth_refresh_token, sizeof(settings->oauth_refresh_token));
    }
    if (strlen(settings->oauth_refresh_token) == 0) {
        nvs_read_str_helper(handle, "token", settings->oauth_refresh_token, sizeof(settings->oauth_refresh_token));
    }

    nvs_close(handle);

    if (strlen(settings->timezone) == 0) {
        strncpy(settings->timezone, DEFAULT_TIMEZONE, sizeof(settings->timezone) - 1);
    }
    if (strlen(settings->weather_location) == 0) {
        strncpy(settings->weather_location, DEFAULT_WEATHER_LOC, sizeof(settings->weather_location) - 1);
    }

    ESP_LOGI(TAG, "Loaded settings: SSID='%s', TZ='%s', WeatherLoc='%s', Brightness=%d%%, HasOAuth=%s",
             settings->wifi_ssid, settings->timezone, settings->weather_location, settings->brightness,
             (strlen(settings->oauth_refresh_token) > 0 && strlen(settings->oauth_client_id) > 0) ? "YES" : "NO");

    return ESP_OK;
}

esp_err_t settings_mgr_save(const app_settings_t *settings)
{
    if (!settings) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_str(handle, "ssid", settings->wifi_ssid);
    nvs_set_str(handle, "pass", settings->wifi_pass);
    nvs_set_str(handle, "tz", settings->timezone);
    nvs_set_str(handle, "wloc", settings->weather_location);
    nvs_set_u8(handle, "bright", settings->brightness <= 100 ? settings->brightness : 100);
    nvs_set_str(handle, "cid", settings->oauth_client_id);
    nvs_set_str(handle, "sec", settings->oauth_client_secret);
    nvs_set_str(handle, "refr", settings->oauth_refresh_token);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        g_settings = *settings;
        ESP_LOGI(TAG, "Settings successfully saved to NVS and active config updated");
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS settings: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t settings_mgr_set_brightness(uint8_t brightness)
{
    if (brightness > 100) brightness = 100;
    g_settings.brightness = brightness;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for brightness update: %s", esp_err_to_name(err));
        return err;
    }
    nvs_set_u8(handle, "bright", brightness);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
