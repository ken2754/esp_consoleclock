#include "weather_client.h"
#include "google_quota.h"
#include "settings_mgr.h"
#include "wifi_portal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "weather_client";

#define WX_REFRESH_BIT          BIT0
#define WX_HTTP_BUF_SIZE        3072
#define WX_REFRESH_INTERVAL_SEC 900  // 15 minutes

static EventGroupHandle_t s_wx_ev = NULL;
static weather_data_t s_wx_data = {
    .valid = false,
    .is_loading = true,
    .location = "Tokyo",
    .cond_short = "CLR",
    .temp_c = 18.0f,
    .humidity = 50,
    .wind_mps = 2.4f,
    .max_c = 21.0f,
    .min_c = 12.0f,
    .pop_pct = 10,
    .tmrw_cond = "PT_CLD",
    .tmrw_max_c = 19.0f,
    .tmrw_min_c = 11.0f,
    .tmrw_pop_pct = 20,
    .last_update_sec = 0
};

typedef struct {
    const char *name;
    float lat;
    float lon;
} city_coord_t;

// Built-in fast lookup table for major Japanese & global cities
static const city_coord_t s_known_cities[] = {
    { "Tokyo",         35.6895f, 139.6917f },
    { "Yokohama",      35.4437f, 139.6380f },
    { "Kawasaki",      35.5308f, 139.7029f },
    { "Saitama",       35.8617f, 139.6455f },
    { "Chiba",         35.6074f, 140.1065f },
    { "Tsukuba",       36.0835f, 140.0764f },
    { "Mito",          36.3659f, 140.4712f },
    { "Utsunomiya",    36.5551f, 139.8828f },
    { "Maebashi",      36.3895f, 139.0634f },
    { "Osaka",         34.6937f, 135.5023f },
    { "Kyoto",         35.0116f, 135.7681f },
    { "Kobe",          34.6901f, 135.1955f },
    { "Nara",          34.6851f, 135.8048f },
    { "Otsu",          35.0179f, 135.8546f },
    { "Wakayama",      34.2305f, 135.1708f },
    { "Nagoya",        35.1815f, 136.9066f },
    { "Shizuoka",      34.9756f, 138.3828f },
    { "Hamamatsu",     34.7108f, 137.7261f },
    { "Gifu",          35.4233f, 136.7607f },
    { "Tsu",           34.7186f, 136.5057f },
    { "Niigata",       37.9162f, 139.0364f },
    { "Kanazawa",      36.5613f, 136.6562f },
    { "Toyama",        36.6959f, 137.2137f },
    { "Fukui",         36.0641f, 136.2195f },
    { "Nagano",        36.6485f, 138.1942f },
    { "Kofu",          35.6621f, 138.5683f },
    { "Sapporo",       43.0618f, 141.3545f },
    { "Hakodate",      41.7687f, 140.7288f },
    { "Asahikawa",     43.7706f, 142.3650f },
    { "Sendai",        38.2682f, 140.8694f },
    { "Aomori",        40.8222f, 140.7474f },
    { "Morioka",       39.7020f, 141.1545f },
    { "Akita",         39.7200f, 140.1026f },
    { "Yamagata",      38.2554f, 140.3396f },
    { "Fukushima",     37.7608f, 140.4747f },
    { "Hiroshima",     34.3853f, 132.4553f },
    { "Okayama",       34.6551f, 133.9195f },
    { "Yamaguchi",     34.1785f, 131.4737f },
    { "Tottori",       35.5011f, 134.2351f },
    { "Matsue",        35.4681f, 133.0484f },
    { "Takamatsu",     34.3428f, 134.0466f },
    { "Matsuyama",     33.8392f, 132.7656f },
    { "Tokushima",     34.0703f, 134.5548f },
    { "Kochi",         33.5597f, 133.5311f },
    { "Fukuoka",       33.5904f, 130.4017f },
    { "Kitakyushu",    33.8835f, 130.8752f },
    { "Saga",          33.2635f, 130.3009f },
    { "Nagasaki",      32.7503f, 129.8779f },
    { "Kumamoto",      32.8031f, 130.7079f },
    { "Oita",          33.2396f, 131.6093f },
    { "Miyazaki",      31.9077f, 131.4202f },
    { "Kagoshima",     31.5966f, 130.5571f },
    { "Naha",          26.2124f, 127.6809f },
    { "Okinawa",       26.2124f, 127.6809f },
    { "New York",      40.7128f, -74.0060f },
    { "San Francisco", 37.7749f, -122.4194f },
    { "Los Angeles",   34.0522f, -118.2437f },
    { "Seattle",       47.6062f, -122.3321f },
    { "London",        51.5074f,  -0.1278f },
    { "Paris",         48.8566f,   2.3522f },
    { "Berlin",        52.5200f,  13.4050f },
    { "Singapore",      1.3521f, 103.8198f },
    { "Seoul",         37.5665f, 126.9780f },
    { "Taipei",        25.0330f, 121.5654f },
    { "Sydney",       -33.8688f, 151.2093f }
};

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_buf_t;

static esp_err_t wx_http_ev_cb(esp_http_client_event_t *evt)
{
    http_buf_t *hb = (http_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && hb && hb->buf) {
        size_t copy_len = evt->data_len;
        if (hb->len + copy_len >= hb->cap) {
            copy_len = (hb->cap - 1 > hb->len) ? (hb->cap - 1 - hb->len) : 0;
        }
        if (copy_len > 0) {
            memcpy(hb->buf + hb->len, evt->data, copy_len);
            hb->len += copy_len;
            hb->buf[hb->len] = '\0';
        }
    }
    return ESP_OK;
}

static const char *wmo_code_to_short(int code)
{
    if (code == 0) return "CLR";
    if (code == 1) return "FEW_CLD";
    if (code == 2) return "PT_CLD";
    if (code == 3) return "OVC";
    if (code == 45 || code == 48) return "FOG";
    if (code >= 51 && code <= 57) return "DRZL";
    if (code == 61 || code == 63 || code == 80 || code == 81) return "RAIN";
    if (code == 65 || code == 66 || code == 67 || code == 82) return "HVY_RN";
    if (code >= 71 && code <= 77) return "SNOW";
    if (code == 85 || code == 86) return "SNW_SHW";
    if (code >= 95) return "TSRA";
    return "CLD";
}

static bool resolve_location_coords(const char *loc_input, float *lat_out, float *lon_out, char *disp_name, size_t name_sz)
{
    if (!loc_input || strlen(loc_input) == 0) {
        loc_input = DEFAULT_WEATHER_LOC;
    }

    // 1. Check if input is "lat,lon"
    float lat = 0.0f, lon = 0.0f;
    if (sscanf(loc_input, "%f,%f", &lat, &lon) == 2 && lat >= -90.0f && lat <= 90.0f && lon >= -180.0f && lon <= 180.0f) {
        *lat_out = lat;
        *lon_out = lon;
        snprintf(disp_name, name_sz, "%.2f,%.2f", lat, lon);
        return true;
    }

    // 2. Check built-in table (case-insensitive)
    for (size_t i = 0; i < sizeof(s_known_cities) / sizeof(s_known_cities[0]); i++) {
        if (strcasecmp(loc_input, s_known_cities[i].name) == 0) {
            *lat_out = s_known_cities[i].lat;
            *lon_out = s_known_cities[i].lon;
            snprintf(disp_name, name_sz, "%s", s_known_cities[i].name);
            return true;
        }
    }

    // 3. Query Open-Meteo Geocoding API
    char encoded[96];
    size_t j = 0;
    for (size_t i = 0; loc_input[i] != '\0' && j + 4 < sizeof(encoded); i++) {
        char c = loc_input[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            encoded[j++] = c;
        } else if (c == ' ') {
            encoded[j++] = '+';
        }
    }
    encoded[j] = '\0';

    char url[256];
    snprintf(url, sizeof(url),
             "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json",
             encoded);

    http_buf_t hb = {
        .buf = malloc(2048),
        .len = 0,
        .cap = 2048
    };
    if (!hb.buf) return false;
    hb.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
        .event_handler = wx_http_ev_cb,
        .user_data = &hb,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    bool ok = false;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        if (esp_http_client_perform(client) == ESP_OK && esp_http_client_get_status_code(client) == 200) {
            cJSON *root = cJSON_Parse(hb.buf);
            if (root) {
                cJSON *results = cJSON_GetObjectItem(root, "results");
                if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0) {
                    cJSON *first = cJSON_GetArrayItem(results, 0);
                    cJSON *j_lat = cJSON_GetObjectItem(first, "latitude");
                    cJSON *j_lon = cJSON_GetObjectItem(first, "longitude");
                    cJSON *j_name = cJSON_GetObjectItem(first, "name");
                    if (cJSON_IsNumber(j_lat) && cJSON_IsNumber(j_lon)) {
                        *lat_out = (float)j_lat->valuedouble;
                        *lon_out = (float)j_lon->valuedouble;
                        if (cJSON_IsString(j_name) && j_name->valuestring) {
                            snprintf(disp_name, name_sz, "%.24s", j_name->valuestring);
                        } else {
                            snprintf(disp_name, name_sz, "%.24s", loc_input);
                        }
                        ok = true;
                    }
                }
                cJSON_Delete(root);
            }
        }
        esp_http_client_cleanup(client);
    }
    free(hb.buf);

    if (!ok) {
        // Fallback to Tokyo if geocoding failed
        *lat_out = 35.6895f;
        *lon_out = 139.6917f;
        snprintf(disp_name, name_sz, "%.24s", loc_input);
    }
    return ok;
}

static bool fetch_open_meteo(void)
{
    const app_settings_t *s = settings_mgr_get();
    const char *loc_cfg = (strlen(s->weather_location) > 0) ? s->weather_location : DEFAULT_WEATHER_LOC;

    float lat = 35.6895f, lon = 139.6917f;
    char resolved_name[32] = "Tokyo";
    resolve_location_coords(loc_cfg, &lat, &lon, resolved_name, sizeof(resolved_name));

    char url[384];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&wind_speed_unit=ms&timezone=auto&forecast_days=2",
             lat, lon);

    http_buf_t hb = {
        .buf = malloc(WX_HTTP_BUF_SIZE),
        .len = 0,
        .cap = WX_HTTP_BUF_SIZE
    };
    if (!hb.buf) return false;
    hb.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .event_handler = wx_http_ev_cb,
        .user_data = &hb,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    bool success = false;
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        if (esp_http_client_perform(client) == ESP_OK && esp_http_client_get_status_code(client) == 200) {
            cJSON *root = cJSON_Parse(hb.buf);
            if (root) {
                cJSON *cur = cJSON_GetObjectItem(root, "current");
                cJSON *daily = cJSON_GetObjectItem(root, "daily");
                if (cur) {
                    strncpy(s_wx_data.location, resolved_name, sizeof(s_wx_data.location) - 1);
                    s_wx_data.location[sizeof(s_wx_data.location) - 1] = '\0';

                    cJSON *t = cJSON_GetObjectItem(cur, "temperature_2m");
                    cJSON *rh = cJSON_GetObjectItem(cur, "relative_humidity_2m");
                    cJSON *wc = cJSON_GetObjectItem(cur, "weather_code");
                    cJSON *ws = cJSON_GetObjectItem(cur, "wind_speed_10m");

                    if (cJSON_IsNumber(t))  s_wx_data.temp_c = (float)t->valuedouble;
                    if (cJSON_IsNumber(rh)) s_wx_data.humidity = rh->valueint;
                    if (cJSON_IsNumber(ws)) s_wx_data.wind_mps = (float)ws->valuedouble;
                    if (cJSON_IsNumber(wc)) {
                        snprintf(s_wx_data.cond_short, sizeof(s_wx_data.cond_short), "%s", wmo_code_to_short(wc->valueint));
                    }

                    if (daily) {
                        cJSON *d_wc  = cJSON_GetObjectItem(daily, "weather_code");
                        cJSON *d_max = cJSON_GetObjectItem(daily, "temperature_2m_max");
                        cJSON *d_min = cJSON_GetObjectItem(daily, "temperature_2m_min");
                        cJSON *d_pop = cJSON_GetObjectItem(daily, "precipitation_probability_max");

                        if (cJSON_IsArray(d_max) && cJSON_GetArraySize(d_max) >= 1) {
                            s_wx_data.max_c = (float)cJSON_GetArrayItem(d_max, 0)->valuedouble;
                            if (cJSON_GetArraySize(d_max) >= 2) {
                                s_wx_data.tmrw_max_c = (float)cJSON_GetArrayItem(d_max, 1)->valuedouble;
                            }
                        }
                        if (cJSON_IsArray(d_min) && cJSON_GetArraySize(d_min) >= 1) {
                            s_wx_data.min_c = (float)cJSON_GetArrayItem(d_min, 0)->valuedouble;
                            if (cJSON_GetArraySize(d_min) >= 2) {
                                s_wx_data.tmrw_min_c = (float)cJSON_GetArrayItem(d_min, 1)->valuedouble;
                            }
                        }
                        if (cJSON_IsArray(d_pop) && cJSON_GetArraySize(d_pop) >= 1) {
                            cJSON *p0 = cJSON_GetArrayItem(d_pop, 0);
                            if (cJSON_IsNumber(p0)) s_wx_data.pop_pct = p0->valueint;
                            if (cJSON_GetArraySize(d_pop) >= 2) {
                                cJSON *p1 = cJSON_GetArrayItem(d_pop, 1);
                                if (cJSON_IsNumber(p1)) s_wx_data.tmrw_pop_pct = p1->valueint;
                            }
                        }
                        if (cJSON_IsArray(d_wc) && cJSON_GetArraySize(d_wc) >= 2) {
                            cJSON *w1 = cJSON_GetArrayItem(d_wc, 1);
                            if (cJSON_IsNumber(w1)) {
                                snprintf(s_wx_data.tmrw_cond, sizeof(s_wx_data.tmrw_cond), "%s", wmo_code_to_short(w1->valueint));
                            }
                        }
                    }

                    time_t now;
                    time(&now);
                    s_wx_data.last_update_sec = (int64_t)now;
                    s_wx_data.valid = true;
                    s_wx_data.is_loading = false;
                    success = true;
                    ESP_LOGI(TAG, "Weather updated for %s: %s %.1fC (Hi:%.1f/Lo:%.1f PoP:%d%%)",
                             s_wx_data.location, s_wx_data.cond_short, s_wx_data.temp_c,
                             s_wx_data.max_c, s_wx_data.min_c, s_wx_data.pop_pct);
                }
                cJSON_Delete(root);
            }
        }
        esp_http_client_cleanup(client);
    }

    free(hb.buf);
    return success;
}

static void weather_task(void *pvParameters)
{
    (void)pvParameters;
    while (1) {
        xEventGroupWaitBits(s_wx_ev, WX_REFRESH_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(WX_REFRESH_INTERVAL_SEC * 1000));

        if (!wifi_portal_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // Yield briefly so high-priority quota_task takes the TLS lock first on simultaneous wakeup
        vTaskDelay(pdMS_TO_TICKS(400));

        google_quota_tls_lock();
        fetch_open_meteo();
        google_quota_tls_unlock();
    }
}

esp_err_t weather_client_init(void)
{
    const app_settings_t *s = settings_mgr_get();
    if (strlen(s->weather_location) > 0) {
        strncpy(s_wx_data.location, s->weather_location, sizeof(s_wx_data.location) - 1);
        s_wx_data.location[sizeof(s_wx_data.location) - 1] = '\0';
    }

    s_wx_ev = xEventGroupCreate();
    if (!s_wx_ev) return ESP_FAIL;

    xTaskCreatePinnedToCore(weather_task, "weather_task", 6144, NULL, 3, NULL, 0);
    return ESP_OK;
}

void weather_client_request_refresh(void)
{
    const app_settings_t *s = settings_mgr_get();
    if (strlen(s->weather_location) > 0) {
        strncpy(s_wx_data.location, s->weather_location, sizeof(s_wx_data.location) - 1);
        s_wx_data.location[sizeof(s_wx_data.location) - 1] = '\0';
    }
    if (s_wx_ev) {
        xEventGroupSetBits(s_wx_ev, WX_REFRESH_BIT);
    }
}

const weather_data_t *weather_client_get_latest(void)
{
    return &s_wx_data;
}
