#include "touch_driver.h"
#include "config.h"
#include <string.h>
#include "esp_log.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

static const char *TAG = "touch_driver";

static spi_device_handle_t s_touch_spi = NULL;
static touch_callback_t s_touch_callback = NULL;

static bool s_is_pressed = false;
static int s_press_count = 0;
static int s_release_count = 0;
static int s_last_screen_x = 0;
static int s_last_screen_y = 0;

static uint16_t xpt2046_read_channel(uint8_t cmd)
{
    if (!s_touch_spi) return 0;

    uint8_t tx_data[3] = { cmd, 0x00, 0x00 };
    uint8_t rx_data[3] = { 0, 0, 0 };

    spi_transaction_t t = {
        .length = 24, // 3 bytes = 24 bits
        .tx_buffer = tx_data,
        .rx_buffer = rx_data,
    };

    esp_err_t ret = spi_device_transmit(s_touch_spi, &t);
    if (ret != ESP_OK) return 0;

    // In XPT2046 12-bit mode: 1 bit busy + 12 bits data (MSB first)
    uint16_t val = ((rx_data[1] << 8) | rx_data[2]) >> 3;
    return val & 0x0FFF;
}

static bool read_touch_raw(int *out_x, int *out_y, int *out_z)
{
    // Command 0xB0: Read Z1 pressure
    uint16_t z1 = xpt2046_read_channel(0xB0);
    if (z1 < 250) {
        return false;
    }

    uint16_t z2 = xpt2046_read_channel(0xC0);
    int32_t z = (int32_t)z1 + 4095 - (int32_t)z2;
    if (z < 250) {
        return false;
    }

    // Command 0x90: Read X coordinate
    uint16_t x = xpt2046_read_channel(0x90);
    // Command 0xD0: Read Y coordinate
    uint16_t y = xpt2046_read_channel(0xD0);

    if (x > 50 && x < 4050 && y > 50 && y < 4050) {
        if (out_x) *out_x = x;
        if (out_y) *out_y = y;
        if (out_z) *out_z = z;
        return true;
    }

    return false;
}

bool touch_driver_is_pressed(void)
{
    return s_is_pressed;
}

bool touch_driver_get_xy(int *x, int *y)
{
    if (x) *x = s_last_screen_x;
    if (y) *y = s_last_screen_y;
    return s_is_pressed;
}

void touch_driver_poll(void)
{
    static int s_poll_div = 0;
    if (++s_poll_div % 2 != 0) {
        return;
    }

    int raw_x = 0, raw_y = 0, raw_z = 0;
    bool raw_pressed = read_touch_raw(&raw_x, &raw_y, &raw_z);

    if (raw_pressed) {
        s_release_count = 0;
        s_press_count++;

        // Convert raw touch to screen coordinates (Landscape 320x240)
        int sx = (int)((3800 - raw_x) * (int64_t)LCD_H_RES / (3800 - 300));
        int sy = (int)((3800 - raw_y) * (int64_t)LCD_V_RES / (3800 - 300));
        if (sx < 0) sx = 0;
        if (sx >= LCD_H_RES) sx = LCD_H_RES - 1;
        if (sy < 0) sy = 0;
        if (sy >= LCD_V_RES) sy = LCD_V_RES - 1;
        s_last_screen_x = sx;
        s_last_screen_y = sy;

        if (s_press_count >= 2 && !s_is_pressed) {
            s_is_pressed = true;
            ESP_LOGI(TAG, "Touch pressed at screen (%d, %d)", s_last_screen_x, s_last_screen_y);
            if (s_touch_callback) {
                s_touch_callback();
            }
        }
    } else {
        s_press_count = 0;
        s_release_count++;
        if (s_release_count >= 2 && s_is_pressed) {
            s_is_pressed = false;
            ESP_LOGD(TAG, "Touch released");
        }
    }
}

esp_err_t touch_driver_init(touch_callback_t on_touch_cb)
{
    s_touch_callback = on_touch_cb;
    s_is_pressed = false;
    s_press_count = 0;
    s_release_count = 0;

    ESP_LOGI(TAG, "Initializing SPI bus for CYD touch (SCLK=%d, MOSI=%d, MISO=%d, CS=%d)...",
             TOUCH_PIN_NUM_CLK, TOUCH_PIN_NUM_MOSI, TOUCH_PIN_NUM_MISO, TOUCH_PIN_NUM_CS);

    spi_bus_config_t buscfg = {
        .sclk_io_num = TOUCH_PIN_NUM_CLK,
        .mosi_io_num = TOUCH_PIN_NUM_MOSI,
        .miso_io_num = TOUCH_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 32,
    };

    esp_err_t ret = spi_bus_initialize(TOUCH_SPI_HOST, &buscfg, SPI_DMA_DISABLED);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize SPI3 bus for touch: %s", esp_err_to_name(ret));
        return ret;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 2 * 1000 * 1000, // 2 MHz
        .mode = 0,
        .spics_io_num = TOUCH_PIN_NUM_CS,
        .queue_size = 1,
    };

    ret = spi_bus_add_device(TOUCH_SPI_HOST, &devcfg, &s_touch_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add touch device to SPI: %s", esp_err_to_name(ret));
        return ret;
    }

    xpt2046_read_channel(0xB0);
    ESP_LOGI(TAG, "Touch driver initialized successfully (XPT2046 on SPI3)");
    return ESP_OK;
}
