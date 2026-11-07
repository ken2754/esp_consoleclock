#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "lvgl.h"

#include "config.h"
#include "settings_mgr.h"
#include "display_driver.h"
#include "touch_driver.h"
#include "sntp_time.h"
#include "google_quota.h"
#include "weather_client.h"
#include "wifi_portal.h"
#include "terminal_ui.h"
#include "console_sim.h"

static const char *TAG = "main";

static void on_touch_event(void)
{
    int tx = 0, ty = 0;
    touch_driver_get_xy(&tx, &ty);
    ESP_LOGI(TAG, "Touch detected at (%d, %d)", tx, ty);

    if (display_driver_get_brightness() == 0) {
        display_driver_cycle_brightness();
        return;
    }

    console_sim_notify_touch(tx, ty);
}

static void on_wifi_state(bool connected, const char *ip_str)
{
    console_sim_notify_wifi(connected, ip_str);
    if (connected) {
        sntp_time_restart();
        google_quota_request_refresh();
        weather_client_request_refresh();
    }
}

static void on_sntp_synced(void)
{
    ESP_LOGI(TAG, "SNTP time synchronized");
}

static void on_quota_updated(const google_quota_data_t *data)
{
    (void)data;
    ESP_LOGD(TAG, "Quota background cache updated");
}

static void gui_task(void *pvParameter)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10));

        // Poll touch screen
        touch_driver_poll();

        // Step console simulator every 10ms for fine-grained human keystroke cadence
        console_sim_tick();

        // Handle LVGL timers and drawing
        terminal_ui_timer_handler();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, " Starting CYD Bash Console Clock & Antigravity Dashboard");
    ESP_LOGI(TAG, "==========================================================");

    // 1. Initialize NVS settings (shares 'agyclock' namespace with esp_agy_clock)
    ESP_ERROR_CHECK(settings_mgr_init());

    // 2. Initialize Display (ST7789 / ILI9341 auto-fallback) & LVGL
    ESP_ERROR_CHECK(display_driver_init());

    // 3. Initialize Touch Controller (XPT2046)
    ESP_ERROR_CHECK(touch_driver_init(on_touch_event));

    // 4. Initialize Terminal Emulator UI & Console Simulator
    ESP_ERROR_CHECK(terminal_ui_init());
    ESP_ERROR_CHECK(console_sim_init());

    // 5. Initialize Wi-Fi and Web Portal
    ESP_ERROR_CHECK(wifi_portal_init(on_wifi_state));

    // 6. Initialize SNTP with saved timezone
    ESP_ERROR_CHECK(sntp_time_init(on_sntp_synced));

    // 7. Initialize Google AI Quota & Weather Forecast fetchers (active in background)
    ESP_ERROR_CHECK(google_quota_init(on_quota_updated));
    ESP_ERROR_CHECK(weather_client_init());

    // 8. Start GUI & Console Simulation task on Core 1
    xTaskCreatePinnedToCore(gui_task, "gui_task", 8192, NULL, 5, NULL, 1);

    ESP_LOGI(TAG, "System initialization complete!");
}
