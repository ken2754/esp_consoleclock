#include "wifi_portal.h"
#include "settings_mgr.h"
#include "config.h"
#include "google_quota.h"
#include "weather_client.h"
#include "sntp_time.h"
#include "display_driver.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_timer.h"

static const char *TAG = "wifi_portal";

static bool s_is_ap_active = false;
static bool s_is_connected = false;
static char s_current_ip[32] = "192.168.4.1";
static int s_remaining_sec = PORTAL_TIMEOUT_SEC;
static httpd_handle_t s_http_server = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_netif_t *s_sta_netif = NULL;
static wifi_state_cb_t s_user_state_cb = NULL;
static esp_timer_handle_t s_countdown_timer = NULL;

static void configure_sta(const char *ssid, const char *pass)
{
    if (!ssid || strlen(ssid) == 0) return;
    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    if (pass) {
        strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password) - 1);
    }
    ESP_LOGI(TAG, "Configuring STA SSID: '%s', pass_len: %zu", ssid, pass ? strlen(pass) : 0);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
}

static const char HTML_PAGE[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>CYD Bash Console Clock Setup</title>"
"<style>"
"body{font-family:monospace;background:#09090b;color:#e4e4e7;margin:0;padding:16px;}"
".card{max-width:520px;margin:0 auto;background:#18181b;padding:24px;border-radius:8px;border:1px solid #27272a;}"
"h2{margin-top:0;color:#4ade80;font-size:1.25rem;text-align:center;}"
"h3{font-size:0.95rem;color:#38bdf8;margin:16px 0 6px 0;}"
"label{display:block;margin-top:12px;font-size:0.85rem;color:#a1a1aa;font-weight:600;}"
"input,textarea,select{width:100%%;padding:10px;margin-top:6px;border-radius:4px;border:1px solid #3f3f46;background:#09090b;color:#4ade80;box-sizing:border-box;font-family:monospace;font-size:0.9rem;}"
"input:focus,textarea:focus,select:focus{border-color:#4ade80;outline:none;}"
"button{width:100%%;padding:12px;margin-top:24px;background:#16a34a;color:#fff;border:none;border-radius:4px;font-weight:bold;cursor:pointer;font-family:monospace;font-size:1rem;}"
"button:hover{background:#15803d;}"
".note{font-size:0.75rem;color:#71717a;margin-top:4px;line-height:1.4;}"
"hr{border:0;border-top:1px solid #27272a;margin:20px 0;}"
"</style></head><body>"
"<div class='card'>"
"<h2>ken@cyd-esp32:~$ ./configure</h2>"
"<form method='POST' action='/save'>"
"<h3>[Wi-Fi &amp; System]</h3>"
"<label>Wi-Fi SSID</label><input type='text' name='ssid' value='%s'>"
"<label>Wi-Fi Password</label><input type='password' name='pass' value='%s'>"
"<label>Timezone (POSIX format)</label><input type='text' name='tz' value='%s' placeholder='JST-9'>"
"<div class='note'>e.g. JST-9 (Japan), EST5EDT (US Eastern), PST8PDT (US Pacific), UTC0</div>"
"<label>Weather Forecast Region / City</label><input type='text' name='wloc' value='%s' placeholder='Tokyo'>"
"<div class='note'>e.g. Tokyo, Yokohama, Osaka, Nagoya, Sapporo, Fukuoka, Sendai, Naha, London, or lat,lon (35.68,139.76)</div>"
"<label>Display Brightness</label>"
"<select name='bright'>"
"<option value='100' %s>100%% (Bright)</option>"
"<option value='50' %s>50%% (Medium)</option>"
"<option value='25' %s>25%% (Night Dim)</option>"
"<option value='0' %s>0%% (Display Off)</option>"
"</select>"
"<hr>"
"<h3>[Antigravity / Google AI Quota]</h3>"
"<div class='note'>Run <code>python3 tools/get_token.py</code> on your PC to retrieve or auto-upload your token and client credentials.</div>"
"<label>OAuth Client ID</label>"
"<input type='text' name='cid' value='%s' placeholder='Client ID (.apps.googleusercontent.com)'>"
"<label>OAuth Client Secret</label>"
"<input type='password' name='sec' value='%s' placeholder='Client Secret'>"
"<label>OAuth Refresh Token</label>"
"<textarea name='refr' rows='3' placeholder='Paste OAuth Refresh Token here (starts with 1//...)'>%s</textarea>"
"<button type='submit'>[ Save &amp; Apply ]</button>"
"</form></div></body></html>";

static void url_decode(char *dst, const char *src, size_t dst_size)
{
    char a, b;
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) && (isxdigit((int)a) && isxdigit((int)b))) {
            if (a >= 'a') a -= 'a'-'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a'-'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[i++] = 16*a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static bool extract_param(const char *post_data, const char *key, char *out, size_t out_len)
{
    char search_key[64];
    snprintf(search_key, sizeof(search_key), "%s=", key);
    const char *start = strstr(post_data, search_key);
    if (!start) {
        return false;
    }
    start += strlen(search_key);
    const char *end = strchr(start, '&');
    size_t val_len = end ? (size_t)(end - start) : strlen(start);
    char *tmp = malloc(val_len + 1);
    if (!tmp) return false;
    memcpy(tmp, start, val_len);
    tmp[val_len] = '\0';
    url_decode(out, tmp, out_len);
    free(tmp);
    return true;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const app_settings_t *s = settings_mgr_get();

    char b100[12] = "", b50[12] = "", b25[12] = "", b0[12] = "";
    if (s->brightness == 0) strcpy(b0, "selected");
    else if (s->brightness <= 25) strcpy(b25, "selected");
    else if (s->brightness <= 50) strcpy(b50, "selected");
    else strcpy(b100, "selected");

    char *resp = malloc(4096);
    if (!resp) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    snprintf(resp, 4096, HTML_PAGE,
             s->wifi_ssid, s->wifi_pass, s->timezone, s->weather_location,
             b100, b50, b25, b0,
             s->oauth_client_id, s->oauth_client_secret, s->oauth_refresh_token);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    free(resp);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 16384) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload invalid size");
        return ESP_FAIL;
    }

    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int cur_len = 0;
    while (cur_len < total_len) {
        int received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) break;
        cur_len += received;
    }
    buf[cur_len] = '\0';

    app_settings_t old_cfg = *settings_mgr_get();
    app_settings_t new_cfg = old_cfg;
    char param_val[1024];

    bool wifi_changed = false;

    if (extract_param(buf, "ssid", param_val, sizeof(param_val)) && strlen(param_val) > 0) {
        if (strcmp(new_cfg.wifi_ssid, param_val) != 0) wifi_changed = true;
        strncpy(new_cfg.wifi_ssid, param_val, sizeof(new_cfg.wifi_ssid) - 1);
    }
    if (extract_param(buf, "pass", param_val, sizeof(param_val))) {
        if (strcmp(new_cfg.wifi_pass, param_val) != 0) wifi_changed = true;
        strncpy(new_cfg.wifi_pass, param_val, sizeof(new_cfg.wifi_pass) - 1);
    }
    if (extract_param(buf, "tz", param_val, sizeof(param_val)) && strlen(param_val) > 0) {
        strncpy(new_cfg.timezone, param_val, sizeof(new_cfg.timezone) - 1);
    }
    if (extract_param(buf, "wloc", param_val, sizeof(param_val))) {
        char *p = param_val;
        while (*p == ' ' || *p == '\r' || *p == '\n') p++;
        char *end = p + strlen(p) - 1;
        while (end > p && (*end == ' ' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        if (strlen(p) > 0) {
            strncpy(new_cfg.weather_location, p, sizeof(new_cfg.weather_location) - 1);
            new_cfg.weather_location[sizeof(new_cfg.weather_location) - 1] = '\0';
        }
    }
    if (extract_param(buf, "bright", param_val, sizeof(param_val))) {
        new_cfg.brightness = (uint8_t)atoi(param_val);
    }
    if (extract_param(buf, "cid", param_val, sizeof(param_val))) {
        char *p = param_val;
        while (*p == ' ' || *p == '\r' || *p == '\n') p++;
        char *end = p + strlen(p) - 1;
        while (end > p && (*end == ' ' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        strncpy(new_cfg.oauth_client_id, p, sizeof(new_cfg.oauth_client_id) - 1);
    }
    if (extract_param(buf, "sec", param_val, sizeof(param_val))) {
        char *p = param_val;
        while (*p == ' ' || *p == '\r' || *p == '\n') p++;
        char *end = p + strlen(p) - 1;
        while (end > p && (*end == ' ' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        strncpy(new_cfg.oauth_client_secret, p, sizeof(new_cfg.oauth_client_secret) - 1);
    }
    if (extract_param(buf, "refr", param_val, sizeof(param_val))) {
        char *p = param_val;
        while (*p == ' ' || *p == '\r' || *p == '\n') p++;
        char *end = p + strlen(p) - 1;
        while (end > p && (*end == ' ' || *end == '\r' || *end == '\n')) {
            *end = '\0';
            end--;
        }
        strncpy(new_cfg.oauth_refresh_token, p, sizeof(new_cfg.oauth_refresh_token) - 1);
    }

    free(buf);

    settings_mgr_save(&new_cfg);

    sntp_time_update_tz(new_cfg.timezone);
    display_driver_set_backlight(new_cfg.brightness);

    const char *resp = "<html><body style='background:#09090b;color:#4ade80;font-family:monospace;padding:30px;text-align:center;'>"
                       "<h2>[OK] Configuration Saved</h2><p>Settings applied to console session.</p>"
                       "<p><a href='/' style='color:#38bdf8;'>&larr; Return</a></p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    google_quota_request_refresh();
    weather_client_request_refresh();

    if (wifi_changed && strlen(new_cfg.wifi_ssid) > 0) {
        configure_sta(new_cfg.wifi_ssid, new_cfg.wifi_pass);
        esp_wifi_connect();
    }

    return ESP_OK;
}

static esp_err_t favicon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t root_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t save_uri = {
    .uri       = "/save",
    .method    = HTTP_POST,
    .handler   = save_post_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t favicon_uri = {
    .uri       = "/favicon.ico",
    .method    = HTTP_GET,
    .handler   = favicon_get_handler,
    .user_ctx  = NULL
};

static void start_webserver(void)
{
    if (s_http_server) return;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 8;
    if (httpd_start(&s_http_server, &config) == ESP_OK) {
        httpd_register_uri_handler(s_http_server, &root_uri);
        httpd_register_uri_handler(s_http_server, &save_uri);
        httpd_register_uri_handler(s_http_server, &favicon_uri);
        ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    }
}

void wifi_portal_start_ap(void)
{
    if (s_is_ap_active) return;
    ESP_LOGI(TAG, "Starting SoftAP mode: SSID='%s'", PORTAL_AP_SSID);
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        esp_netif_ip_info_t ip_info;
        esp_netif_set_ip4_addr(&ip_info.ip, 192, 168, 4, 1);
        esp_netif_set_ip4_addr(&ip_info.gw, 192, 168, 4, 1);
        esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_ip_info(s_ap_netif, &ip_info);
        esp_netif_dhcps_start(s_ap_netif);
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t ap_config = {
        .ap = {
            .ssid = PORTAL_AP_SSID,
            .ssid_len = strlen(PORTAL_AP_SSID),
            .channel = PORTAL_AP_CHANNEL,
            .password = PORTAL_AP_PASS,
            .max_connection = PORTAL_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
            .beacon_interval = 100,
        },
    };
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    s_is_ap_active = true;
    s_remaining_sec = PORTAL_TIMEOUT_SEC;
    if (s_user_state_cb && !s_is_connected) {
        s_user_state_cb(false, PORTAL_IP_ADDR);
    }
}

void wifi_portal_stop_ap(void)
{
    if (!s_is_ap_active) return;
    if (!s_is_connected) {
        s_remaining_sec = PORTAL_TIMEOUT_SEC;
        return;
    }
    s_is_ap_active = false;
    ESP_LOGI(TAG, "Shutting down SoftAP after timeout");
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (s_countdown_timer) {
        esp_timer_stop(s_countdown_timer);
        esp_timer_delete(s_countdown_timer);
        s_countdown_timer = NULL;
    }
}

static void countdown_timer_cb(void *arg)
{
    if (s_remaining_sec > 0) {
        s_remaining_sec--;
    }
    if (s_remaining_sec <= 0) {
        wifi_portal_stop_ap();
    }

    static int s_diag_cnt = 0;
    static int s_dhcp_wait_sec = 0;
    if (!s_is_connected && s_sta_netif && ++s_diag_cnt >= 3) {
        s_diag_cnt = 0;
        esp_netif_ip_info_t ip_info = {0};
        esp_netif_get_ip_info(s_sta_netif, &ip_info);
        if (ip_info.ip.addr == 0 && esp_netif_is_netif_up(s_sta_netif)) {
            s_dhcp_wait_sec += 3;
            if (s_dhcp_wait_sec >= 15 && !s_is_ap_active) {
                ESP_LOGW(TAG, "DHCP lease taking longer than %d sec -> Starting SoftAP fallback", s_dhcp_wait_sec);
                wifi_portal_start_ap();
            }
        }
    }
}

static esp_timer_handle_t s_reconnect_timer = NULL;

static void reconnect_timer_cb(void *arg)
{
    if (!s_is_connected && strlen(g_settings.wifi_ssid) > 0) {
        ESP_LOGI(TAG, "Reconnecting to Wi-Fi SSID '%s'...", g_settings.wifi_ssid);
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (strlen(g_settings.wifi_ssid) > 0) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        esp_netif_dhcp_status_t status = 0;
        if (esp_netif_dhcpc_get_status(s_sta_netif, &status) == ESP_OK && status != ESP_NETIF_DHCP_STARTED) {
            esp_netif_dhcpc_start(s_sta_netif);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        if (s_user_state_cb) s_user_state_cb(false, NULL);
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, 3000 * 1000);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_current_ip, sizeof(s_current_ip));
        ESP_LOGI(TAG, "Got IP address: %s", s_current_ip);
        s_is_connected = true;
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
        }
        if (s_user_state_cb) s_user_state_cb(true, s_current_ip);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        s_is_connected = false;
        if (s_user_state_cb) s_user_state_cb(false, NULL);
    }
}

esp_err_t wifi_portal_init(wifi_state_cb_t state_cb)
{
    s_user_state_cb = state_cb;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    bool has_creds = (strlen(g_settings.wifi_ssid) > 0);
    if (has_creds) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        configure_sta(g_settings.wifi_ssid, g_settings.wifi_pass);
        s_is_ap_active = false;
        s_remaining_sec = 0;
    } else {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        esp_netif_ip_info_t ip_info;
        esp_netif_set_ip4_addr(&ip_info.ip, 192, 168, 4, 1);
        esp_netif_set_ip4_addr(&ip_info.gw, 192, 168, 4, 1);
        esp_netif_set_ip4_addr(&ip_info.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_set_ip_info(s_ap_netif, &ip_info);
        esp_netif_dhcps_start(s_ap_netif);

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        wifi_config_t ap_config = {
            .ap = {
                .ssid = PORTAL_AP_SSID,
                .ssid_len = strlen(PORTAL_AP_SSID),
                .channel = PORTAL_AP_CHANNEL,
                .password = PORTAL_AP_PASS,
                .max_connection = PORTAL_MAX_CONN,
                .authmode = WIFI_AUTH_OPEN,
                .beacon_interval = 100,
            },
        };
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
        s_is_ap_active = true;
        s_remaining_sec = PORTAL_TIMEOUT_SEC;
    }

    const esp_timer_create_args_t reconn_args = {
        .callback = &reconnect_timer_cb,
        .name = "wifi_reconnect"
    };
    ESP_ERROR_CHECK(esp_timer_create(&reconn_args, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    start_webserver();

    const esp_timer_create_args_t timer_args = {
        .callback = &countdown_timer_cb,
        .name = "portal_countdown"
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_countdown_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_countdown_timer, 1000 * 1000));

    if (s_is_ap_active && s_user_state_cb) {
        s_user_state_cb(false, PORTAL_IP_ADDR);
    }

    return ESP_OK;
}

bool wifi_portal_is_ap_active(void)
{
    return s_is_ap_active;
}

int wifi_portal_get_remaining_sec(void)
{
    return s_remaining_sec;
}

bool wifi_portal_is_connected(void)
{
    return s_is_connected;
}

const char *wifi_portal_get_ip(void)
{
    return s_current_ip;
}
