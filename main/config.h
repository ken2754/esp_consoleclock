#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// CYD 2.8" ESP32-2432S028 Pin Configuration
#define LCD_HOST                SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ      (40 * 1000 * 1000)
#define LCD_PIN_NUM_SCLK        14
#define LCD_PIN_NUM_MOSI        13
#define LCD_PIN_NUM_MISO        12
#define LCD_PIN_NUM_DC          2
#define LCD_PIN_NUM_CS          15
#define LCD_PIN_NUM_RST         -1
#define LCD_PIN_NUM_BK_LIGHT    21

// Display dimensions (Landscape)
#define LCD_H_RES               320
#define LCD_V_RES               240

// Console Grid Dimensions (using crisp non-bold 6x8 monospace font lv_font_term_6x8)
#define TERM_COLS               52
#define TERM_ROWS               30
#define TERM_LINE_HEIGHT        8
#define TERM_CHAR_WIDTH         6

// CYD 2.8" ESP32-2432S028 Touch (XPT2046) Configuration
#define TOUCH_SPI_HOST          SPI3_HOST
#define TOUCH_PIN_NUM_CLK       25
#define TOUCH_PIN_NUM_MOSI      32
#define TOUCH_PIN_NUM_MISO      39
#define TOUCH_PIN_NUM_CS        33
#define TOUCH_PIN_NUM_IRQ       36

// CYD Onboard RGB LED (Active LOW)
#define LED_PIN_RED             4
#define LED_PIN_GREEN           16
#define LED_PIN_BLUE            17

// SoftAP Configuration
#define PORTAL_AP_SSID          "CYD-AgyClock-Setup"
#define PORTAL_AP_PASS          ""          // Open AP for initial configuration
#define PORTAL_AP_CHANNEL       1
#define PORTAL_MAX_CONN         4
#define PORTAL_TIMEOUT_SEC      180         // 3 minutes (180 seconds)
#define PORTAL_IP_ADDR          "192.168.4.1"

// Default Settings
#define DEFAULT_TIMEZONE        "JST-9"
#define DEFAULT_NTP_SERVER      "pool.ntp.org"
#define DEFAULT_BRIGHTNESS      100         // 0%, 25%, 50%, 100% loop
#define DEFAULT_WEATHER_LOC     "Tokyo"

// Google OAuth & CloudCode Endpoints
#define GOOGLE_OAUTH_TOKEN_URL          "https://oauth2.googleapis.com/token"
#define GOOGLE_QUOTA_SUMMARY_ENDPOINT   "https://daily-cloudcode-pa.googleapis.com/v1internal:retrieveUserQuotaSummary"
#define DEFAULT_OAUTH_CLIENT_ID         ""
#define DEFAULT_OAUTH_CLIENT_SECRET     ""
#define ANTIGRAVITY_USER_AGENT          "antigravity/cli/1.4.2 (aidev_client; os_type=linux; arch=amd64; cl=1001591392; auth_method=consumer)"
#define ANTIGRAVITY_PROJECT_ID          "aicode-consumers"

// Quota Info Structure
typedef struct {
    bool valid;
    char name[32];              // Display name
    float remaining_fraction;   // 0.0 to 1.0 (e.g. 0.85 = 85%)
    int64_t reset_epoch_sec;    // Unix timestamp in seconds
    char reset_time_str[32];    // Formatted reset time or relative time string
} quota_window_t;

typedef struct {
    bool is_loading;
    bool has_error;
    char error_msg[64];
    quota_window_t limit_5h;          // Gemini Models 5-Hour Limit
    quota_window_t limit_weekly;      // Gemini Models Weekly Limit
    quota_window_t limit_claude_5h;   // Claude Models 5-Hour Limit
    quota_window_t limit_claude_wk;   // Claude Models Weekly Limit
    quota_window_t limit_argon;       // Gemini 4 Argon Weekly Limit
    int64_t last_update_sec;
} google_quota_data_t;

#ifdef __cplusplus
}
#endif
