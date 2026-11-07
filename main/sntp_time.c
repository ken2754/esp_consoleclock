#include "sntp_time.h"
#include "settings_mgr.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "sntp_time";
static bool s_is_synced = false;
static sntp_sync_cb_t s_sync_cb = NULL;

static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time synchronized successfully");
    s_is_synced = true;
    if (s_sync_cb) {
        s_sync_cb();
    }
}

void sntp_time_update_tz(const char *tz_string)
{
    if (!tz_string || strlen(tz_string) == 0) {
        tz_string = DEFAULT_TIMEZONE;
    }
    setenv("TZ", tz_string, 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to '%s'", tz_string);
}

void sntp_time_restart(void)
{
    ESP_LOGI(TAG, "Restarting SNTP client to force sync...");
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, DEFAULT_NTP_SERVER);
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "ntp.nict.jp");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
}

esp_err_t sntp_time_init(sntp_sync_cb_t cb)
{
    s_sync_cb = cb;
    const app_settings_t *s = settings_mgr_get();
    sntp_time_update_tz(s->timezone);
    sntp_time_restart();
    return ESP_OK;
}

bool sntp_time_is_synced(void)
{
    return s_is_synced;
}

bool sntp_time_get_local(struct tm *out_tm)
{
    time_t now;
    time(&now);
    if (now < 1600000000) { // Prior to 2020: not synced
        return false;
    }
    s_is_synced = true;
    if (out_tm) {
        localtime_r(&now, out_tm);
    }
    return true;
}

void sntp_time_get_current(clock_display_time_t *out_time)
{
    if (!out_time) return;
    memset(out_time, 0, sizeof(clock_display_time_t));

    time_t now;
    time(&now);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    if (timeinfo.tm_year < (2024 - 1900)) {
        out_time->is_synced = false;
    } else {
        out_time->is_synced = true;
        s_is_synced = true;
    }

    int raw_hour = timeinfo.tm_hour;
    out_time->is_pm = (raw_hour >= 12);
    out_time->hour12 = raw_hour % 12;
    if (out_time->hour12 == 0) {
        out_time->hour12 = 12;
    }
    out_time->min = timeinfo.tm_min;

    strcpy(out_time->am_pm, out_time->is_pm ? "PM" : "AM");
    snprintf(out_time->time_str, sizeof(out_time->time_str), "%2d:%02d", out_time->hour12, out_time->min);
    strftime(out_time->date_str, sizeof(out_time->date_str), "%a, %b %d", &timeinfo);
}
