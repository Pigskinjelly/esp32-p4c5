#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_st7123.h"

#include "app_touch.h"
#include "app_lcd.h"

static const char *TAG = "app_touch";

esp_err_t app_touch_init(i2c_master_bus_handle_t i2c_bus, esp_lcd_touch_handle_t *out)
{
    if (i2c_bus == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io), TAG,
                        "create touch io failed");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = -1,   /* 本板 RST 走 GPIO6，但该脚与 C5 的 IO 复用，悬空最安全 */
        .int_gpio_num = -1,   /* 轮询方式，不依赖 INT */
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    esp_err_t ret = esp_lcd_touch_new_i2c_st7123(tp_io, &tp_cfg, out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "st7123 init failed: 0x%x", ret);
        return ret;
    }

    ESP_LOGI(TAG, "touch ready (ST7123 @ I2C, SDA=7 SCL=8)");
    return ESP_OK;
}
