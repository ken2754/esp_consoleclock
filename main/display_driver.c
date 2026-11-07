#include "display_driver.h"
#include "config.h"
#include "settings_mgr.h"
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_timer.h"

static const char *TAG = "display_driver";

static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
static lcd_driver_type_t s_driver_type = DRIVER_UNKNOWN;

static lv_disp_drv_t s_disp_drv;
static lv_disp_draw_buf_t s_disp_buf;
static lv_color_t *s_buf1 = NULL;
static lv_color_t *s_buf2 = NULL;

#define DISP_BUF_SIZE (LCD_H_RES * 40) // 40 lines buffer

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

static void lvgl_increase_tick(void *arg)
{
    lv_tick_inc(2); // 2ms
}

static void backlight_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_10_BIT,
        .freq_hz          = 5000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LCD_PIN_NUM_BK_LIGHT,
        .duty           = 0,
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
}

static uint8_t s_current_brightness = 100;

void display_driver_set_backlight(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_current_brightness = percent;
    uint32_t duty = (percent * 1023) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

uint8_t display_driver_get_brightness(void)
{
    return s_current_brightness;
}

uint8_t display_driver_cycle_brightness(void)
{
    uint8_t next;
    if (s_current_brightness == 0) {
        next = 25;
    } else if (s_current_brightness <= 25) {
        next = 50;
    } else if (s_current_brightness <= 50) {
        next = 100;
    } else {
        next = 0;
    }

    display_driver_set_backlight(next);
    settings_mgr_set_brightness(next);
    ESP_LOGI(TAG, "Brightness cycled to %d%%", next);
    return next;
}

lcd_driver_type_t display_driver_get_type(void)
{
    return s_driver_type;
}

esp_err_t display_driver_init(void)
{
    ESP_LOGI(TAG, "Initializing Backlight PWM on GPIO %d", LCD_PIN_NUM_BK_LIGHT);
    backlight_init();
    display_driver_set_backlight(0);

    ESP_LOGI(TAG, "Initializing SPI bus for CYD LCD...");
    spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_NUM_SCLK,
        .mosi_io_num = LCD_PIN_NUM_MOSI,
        .miso_io_num = LCD_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISP_BUF_SIZE * sizeof(lv_color_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_NUM_DC,
        .cs_gpio_num = LCD_PIN_NUM_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = notify_lvgl_flush_ready,
        .user_ctx = &s_disp_drv,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &s_io_handle));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };

    ESP_LOGI(TAG, "Initializing display controller (ST7789 / ILI9341 fallback)...");
    esp_lcd_panel_dev_config_t st7789_config = {
        .reset_gpio_num = LCD_PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_err_t ret = esp_lcd_new_panel_st7789(s_io_handle, &st7789_config, &s_panel_handle);
    if (ret == ESP_OK) {
        ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
        s_driver_type = DRIVER_ST7789;
        ESP_LOGI(TAG, "Successfully initialized display as ST7789!");
    } else {
        ESP_LOGW(TAG, "ST7789 init failed (%s), trying ILI9341 fallback...", esp_err_to_name(ret));
        ret = esp_lcd_new_panel_ili9341(s_io_handle, &panel_config, &s_panel_handle);
        if (ret == ESP_OK) {
            ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
            ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
            s_driver_type = DRIVER_ILI9341;
            ESP_LOGI(TAG, "Successfully initialized display as ILI9341!");
        } else {
            ESP_LOGE(TAG, "ILI9341 fallback also failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    // Configure Landscape orientation for CYD (320x240 landscape)
    esp_lcd_panel_swap_xy(s_panel_handle, true);
    esp_lcd_panel_mirror(s_panel_handle, false, true);
    esp_lcd_panel_invert_color(s_panel_handle, false);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);

    ESP_LOGI(TAG, "Initializing LVGL library...");
    lv_init();

    s_buf1 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    s_buf2 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(s_buf1 != NULL);

    lv_disp_draw_buf_init(&s_disp_buf, s_buf1, s_buf2, DISP_BUF_SIZE);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_disp_buf;
    s_disp_drv.user_data = s_panel_handle;
    lv_disp_drv_register(&s_disp_drv);

    // Register LVGL tick timer (every 2ms)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &lvgl_increase_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 2000)); // 2ms

    const app_settings_t *s = settings_mgr_get();
    uint8_t initial_bright = (s->brightness <= 100) ? s->brightness : DEFAULT_BRIGHTNESS;
    display_driver_set_backlight(initial_bright);

    return ESP_OK;
}
