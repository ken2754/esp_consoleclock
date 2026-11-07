#include "google_quota.h"
#include "settings_mgr.h"
#include "wifi_portal.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "google_quota";

static google_quota_data_t s_quota_data = {0};
static google_quota_cb_t s_callback = NULL;
static TaskHandle_t s_task_handle = NULL;
static SemaphoreHandle_t s_tls_mux = NULL;
static volatile bool s_is_active = true; // Active by default for console clock

static char s_access_token[1024] = {0};
static int64_t s_token_expires_at = 0;

#define MAX_HTTP_RECV_BUFFER 4096

void google_quota_tls_lock(void)
{
    if (s_tls_mux) {
        xSemaphoreTake(s_tls_mux, portMAX_DELAY);
    }
}

void google_quota_tls_unlock(void)
{
    if (s_tls_mux) {
        xSemaphoreGive(s_tls_mux);
    }
}

typedef struct {
    char *buffer;
    int buffer_len;
} http_response_buffer_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_buffer_t *resp = (http_response_buffer_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp && resp->buffer) {
            if (resp->buffer_len + evt->data_len < MAX_HTTP_RECV_BUFFER - 1) {
                memcpy(resp->buffer + resp->buffer_len, evt->data, evt->data_len);
                resp->buffer_len += evt->data_len;
                resp->buffer[resp->buffer_len] = '\0';
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static void url_encode(char *dst, size_t dst_len, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 4 < dst_len; s++) {
        unsigned char c = (unsigned char)src[s];
        if (isalnum((int)c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[d++] = c;
        } else {
            dst[d++] = '%';
            dst[d++] = hex[c >> 4];
            dst[d++] = hex[c & 0x0F];
        }
    }
    dst[d] = '\0';
}

// Portable Howard Hinnant algorithm to convert UTC struct tm to Unix epoch time
static time_t my_timegm(struct tm *tm)
{
    int y = tm->tm_year + 1900;
    int m = tm->tm_mon + 1;
    if (m <= 2) {
        y -= 1;
        m += 12;
    }
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m - 3) + 2) / 5 + tm->tm_mday - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)(era * 146097 + (int)doe - 719468);
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

static void parse_reset_time(const char *iso_time, quota_window_t *window, bool is_5h)
{
    if (!iso_time || strlen(iso_time) < 19) {
        strcpy(window->reset_time_str, "Active");
        return;
    }
    int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (sscanf(iso_time, "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) >= 5) {
        struct tm tm_utc = {0};
        tm_utc.tm_year = year - 1900;
        tm_utc.tm_mon = mon - 1;
        tm_utc.tm_mday = day;
        tm_utc.tm_hour = hour;
        tm_utc.tm_min = min;
        tm_utc.tm_sec = sec;
        time_t utc_epoch = my_timegm(&tm_utc);
        window->reset_epoch_sec = (int64_t)utc_epoch;

        struct tm tm_local = {0};
        localtime_r(&utc_epoch, &tm_local);

        if (is_5h) {
            snprintf(window->reset_time_str, sizeof(window->reset_time_str),
                     "Reset %02d:%02d", tm_local.tm_hour, tm_local.tm_min);
        } else {
            snprintf(window->reset_time_str, sizeof(window->reset_time_str),
                     "Reset %d/%d %02d:%02d", tm_local.tm_mon + 1, tm_local.tm_mday,
                     tm_local.tm_hour, tm_local.tm_min);
        }
    } else {
        strcpy(window->reset_time_str, "Active");
    }
}

static bool refresh_oauth_access_token(void)
{
    const char *refr = g_settings.oauth_refresh_token;
    const char *cid = g_settings.oauth_client_id;
    const char *sec = g_settings.oauth_client_secret;
    if (!refr || strlen(refr) == 0 || !cid || strlen(cid) == 0 || !sec || strlen(sec) == 0) {
        ESP_LOGW(TAG, "OAuth credentials incomplete in settings.");
        return false;
    }

    int64_t now_sec = (int64_t)time(NULL);
    if (strlen(s_access_token) > 0 && now_sec < (s_token_expires_at - 120)) {
        return true;
    }

    ESP_LOGI(TAG, "Refreshing Google OAuth Access Token via %s...", GOOGLE_OAUTH_TOKEN_URL);

    char *resp_buf = malloc(MAX_HTTP_RECV_BUFFER);
    if (!resp_buf) {
        ESP_LOGE(TAG, "Failed to allocate OAuth recv buffer");
        return false;
    }
    memset(resp_buf, 0, MAX_HTTP_RECV_BUFFER);
    http_response_buffer_t resp = { .buffer = resp_buf, .buffer_len = 0 };

    esp_http_client_config_t config = {
        .url = GOOGLE_OAUTH_TOKEN_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp_buf);
        return false;
    }

    char enc_cid[128];
    char enc_sec[128];
    char enc_tok[1024];
    url_encode(enc_cid, sizeof(enc_cid), cid);
    url_encode(enc_sec, sizeof(enc_sec), sec);
    url_encode(enc_tok, sizeof(enc_tok), refr);

    char post_data[1500];
    snprintf(post_data, sizeof(post_data),
             "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
             enc_cid, enc_sec, enc_tok);

    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool success = false;
    if (err == ESP_OK && status_code == 200 && resp.buffer_len > 0) {
        cJSON *root = cJSON_Parse(resp.buffer);
        if (root) {
            cJSON *tok_item = cJSON_GetObjectItem(root, "access_token");
            cJSON *exp_item = cJSON_GetObjectItem(root, "expires_in");
            if (tok_item && tok_item->valuestring && strlen(tok_item->valuestring) > 0) {
                strncpy(s_access_token, tok_item->valuestring, sizeof(s_access_token) - 1);
                int exp_sec = exp_item ? exp_item->valueint : 3600;
                s_token_expires_at = (int64_t)time(NULL) + exp_sec;
                ESP_LOGI(TAG, "OAuth access token refreshed successfully! (valid for %d sec)", exp_sec);
                success = true;
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGE(TAG, "OAuth token refresh failed: err=%s, status=%d",
                 esp_err_to_name(err), status_code);
    }

    free(resp_buf);
    return success;
}

static bool fetch_cloudcode_quota_summary(void)
{
    if (!refresh_oauth_access_token()) {
        s_quota_data.has_error = true;
        if (strlen(g_settings.oauth_refresh_token) == 0) {
            strncpy(s_quota_data.error_msg, "Configure OAuth in Portal", sizeof(s_quota_data.error_msg) - 1);
        } else {
            strncpy(s_quota_data.error_msg, "OAuth Token Expired", sizeof(s_quota_data.error_msg) - 1);
        }
        return false;
    }

    ESP_LOGI(TAG, "Fetching Antigravity User Quota Summary via %s...", GOOGLE_QUOTA_SUMMARY_ENDPOINT);

    char *resp_buf = malloc(MAX_HTTP_RECV_BUFFER);
    if (!resp_buf) {
        ESP_LOGE(TAG, "Failed to allocate Quota recv buffer");
        return false;
    }
    memset(resp_buf, 0, MAX_HTTP_RECV_BUFFER);
    http_response_buffer_t resp = { .buffer = resp_buf, .buffer_len = 0 };

    esp_http_client_config_t config = {
        .url = GOOGLE_QUOTA_SUMMARY_ENDPOINT,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp_buf);
        return false;
    }

    char auth_hdr[1100];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", s_access_token);

    const char post_body[] = "{\"project\":\"" ANTIGRAVITY_PROJECT_ID "\"}";

    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "User-Agent", ANTIGRAVITY_USER_AGENT);
    esp_http_client_set_post_field(client, post_body, strlen(post_body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool parsed_success = false;

    if (err == ESP_OK && status_code == 200 && resp.buffer_len > 0) {
        cJSON *root = cJSON_Parse(resp.buffer);
        if (root) {
            cJSON *groups = cJSON_GetObjectItem(root, "groups");
            if (groups && cJSON_IsArray(groups)) {
                int num_groups = cJSON_GetArraySize(groups);
                for (int g = 0; g < num_groups; g++) {
                    cJSON *group = cJSON_GetArrayItem(groups, g);
                    cJSON *grp_name = cJSON_GetObjectItem(group, "displayName");
                    const char *grp_str = grp_name && grp_name->valuestring ? grp_name->valuestring : "";
                    cJSON *buckets = cJSON_GetObjectItem(group, "buckets");
                    if (!buckets || !cJSON_IsArray(buckets)) continue;

                    int num_buckets = cJSON_GetArraySize(buckets);
                    for (int b = 0; b < num_buckets; b++) {
                        cJSON *bucket = cJSON_GetArrayItem(buckets, b);
                        cJSON *b_id = cJSON_GetObjectItem(bucket, "bucketId");
                        cJSON *b_name = cJSON_GetObjectItem(bucket, "displayName");
                        cJSON *b_frac = cJSON_GetObjectItem(bucket, "remainingFraction");
                        cJSON *b_reset = cJSON_GetObjectItem(bucket, "resetTime");

                        const char *id_str = b_id && b_id->valuestring ? b_id->valuestring : "";
                        const char *name_str = b_name && b_name->valuestring ? b_name->valuestring : "";
                        float frac = b_frac ? (float)b_frac->valuedouble : 0.0f;
                        if (frac < 0.0f) frac = 0.0f;
                        if (frac > 1.0f) frac = 1.0f;
                        const char *reset_str = b_reset && b_reset->valuestring ? b_reset->valuestring : "";

                        if (strstr(grp_str, "Gemini Models") != NULL || strcmp(grp_str, "Gemini Models") == 0) {
                            if (strcmp(id_str, "gemini-5h") == 0 || strstr(name_str, "Five Hour") != NULL) {
                                s_quota_data.limit_5h.valid = true;
                                strncpy(s_quota_data.limit_5h.name, "gemini-5h", sizeof(s_quota_data.limit_5h.name) - 1);
                                s_quota_data.limit_5h.remaining_fraction = frac;
                                parse_reset_time(reset_str, &s_quota_data.limit_5h, true);
                                parsed_success = true;
                            } else if (strcmp(id_str, "gemini-weekly") == 0 || strstr(name_str, "Weekly") != NULL) {
                                s_quota_data.limit_weekly.valid = true;
                                strncpy(s_quota_data.limit_weekly.name, "gemini-wk", sizeof(s_quota_data.limit_weekly.name) - 1);
                                s_quota_data.limit_weekly.remaining_fraction = frac;
                                parse_reset_time(reset_str, &s_quota_data.limit_weekly, false);
                                parsed_success = true;
                            }
                        } else if (strstr(grp_str, "Claude") != NULL) {
                            if (strcmp(id_str, "3p-5h") == 0 || strstr(name_str, "Five Hour") != NULL) {
                                s_quota_data.limit_claude_5h.valid = true;
                                strncpy(s_quota_data.limit_claude_5h.name, "claude-5h", sizeof(s_quota_data.limit_claude_5h.name) - 1);
                                s_quota_data.limit_claude_5h.remaining_fraction = frac;
                                parse_reset_time(reset_str, &s_quota_data.limit_claude_5h, true);
                                parsed_success = true;
                            } else if (strcmp(id_str, "3p-weekly") == 0 || strstr(name_str, "Weekly") != NULL) {
                                s_quota_data.limit_claude_wk.valid = true;
                                strncpy(s_quota_data.limit_claude_wk.name, "claude-wk", sizeof(s_quota_data.limit_claude_wk.name) - 1);
                                s_quota_data.limit_claude_wk.remaining_fraction = frac;
                                parse_reset_time(reset_str, &s_quota_data.limit_claude_wk, false);
                                parsed_success = true;
                            }
                        } else if (strstr(grp_str, "Argon") != NULL || strcmp(grp_str, "Gemini 4 Argon") == 0) {
                            if (strcmp(id_str, "gemini-argon-wtu-weekly") == 0 || strstr(name_str, "Argon") != NULL || strstr(name_str, "Weekly") != NULL) {
                                s_quota_data.limit_argon.valid = true;
                                strncpy(s_quota_data.limit_argon.name, "argon-wk", sizeof(s_quota_data.limit_argon.name) - 1);
                                s_quota_data.limit_argon.remaining_fraction = frac;
                                parse_reset_time(reset_str, &s_quota_data.limit_argon, false);
                                parsed_success = true;
                            }
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
    } else {
        ESP_LOGE(TAG, "CloudCode Quota API failed: err=%s, status=%d",
                 esp_err_to_name(err), status_code);
        s_quota_data.has_error = true;
        if (status_code == 401 || status_code == 403) {
            strncpy(s_quota_data.error_msg, "Auth Error / Check Token", sizeof(s_quota_data.error_msg) - 1);
            s_access_token[0] = '\0';
            s_token_expires_at = 0;
        } else {
            snprintf(s_quota_data.error_msg, sizeof(s_quota_data.error_msg), "HTTP %d Error", status_code);
        }
    }

    if (parsed_success) {
        s_quota_data.has_error = false;
        s_quota_data.last_update_sec = (int64_t)time(NULL);
    }

    free(resp_buf);
    return parsed_success;
}

static void quota_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Quota polling task started");
    while (1) {
        if (!s_is_active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (wifi_portal_is_connected()) {
            s_quota_data.is_loading = true;
            if (s_callback) s_callback(&s_quota_data);

            google_quota_tls_lock();
            bool ok = fetch_cloudcode_quota_summary();
            google_quota_tls_unlock();

            s_quota_data.is_loading = false;
            if (ok) {
                s_quota_data.has_error = false;
            }
            if (s_callback) s_callback(&s_quota_data);
        } else {
            s_quota_data.has_error = true;
            strncpy(s_quota_data.error_msg, "Wi-Fi Disconnected", sizeof(s_quota_data.error_msg) - 1);
            if (s_callback) s_callback(&s_quota_data);
        }

        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));
    }
}

esp_err_t google_quota_init(google_quota_cb_t callback)
{
    if (!s_tls_mux) {
        s_tls_mux = xSemaphoreCreateMutex();
    }
    s_callback = callback;
    s_is_active = true;
    memset(&s_quota_data, 0, sizeof(s_quota_data));
    s_quota_data.limit_5h.remaining_fraction = 1.0f;
    s_quota_data.limit_weekly.remaining_fraction = 1.0f;
    s_quota_data.limit_claude_5h.remaining_fraction = 1.0f;
    s_quota_data.limit_claude_wk.remaining_fraction = 1.0f;
    s_quota_data.limit_argon.remaining_fraction = 1.0f;
    strcpy(s_quota_data.limit_5h.name, "gemini-5h");
    strcpy(s_quota_data.limit_weekly.name, "gemini-wk");
    strcpy(s_quota_data.limit_claude_5h.name, "claude-5h");
    strcpy(s_quota_data.limit_claude_wk.name, "claude-wk");
    strcpy(s_quota_data.limit_argon.name, "argon-wk");
    strcpy(s_quota_data.limit_5h.reset_time_str, "pending");
    strcpy(s_quota_data.limit_weekly.reset_time_str, "pending");
    strcpy(s_quota_data.limit_claude_5h.reset_time_str, "pending");
    strcpy(s_quota_data.limit_claude_wk.reset_time_str, "pending");
    strcpy(s_quota_data.limit_argon.reset_time_str, "pending");

    BaseType_t ret = xTaskCreate(quota_task, "quota_task", 8192, NULL, 5, &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create quota_task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void google_quota_set_active(bool active)
{
    s_is_active = active;
    if (active && s_task_handle) {
        xTaskNotifyGive(s_task_handle);
    }
}

bool google_quota_is_active(void)
{
    return s_is_active;
}

void google_quota_request_refresh(void)
{
    s_access_token[0] = '\0';
    s_token_expires_at = 0;
    if (s_is_active && s_task_handle) {
        xTaskNotifyGive(s_task_handle);
    }
}

const google_quota_data_t* google_quota_get_latest(void)
{
    return &s_quota_data;
}
