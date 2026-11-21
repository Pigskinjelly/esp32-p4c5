/*
 * 触摸：ST7102 显示屏为 TDDI 方案，触摸控制器走 Sitronix ST7123 协议。
 * 依据：OSPTEK 官方 LVGL 示例使用 esp_lcd_touch_st7123，SDA=GPIO7 / SCL=GPIO8，
 * RST/INT 均不接（悬空），轮询方式读取。
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 在已有的共享 I2C 总线上挂 ST7123 触摸
 *
 * @param i2c_bus   已初始化的 I2C 主机总线（与摄像头/ES8311 共用）
 * @param out       返回触摸句柄
 */
esp_err_t app_touch_init(i2c_master_bus_handle_t i2c_bus, esp_lcd_touch_handle_t *out);

#ifdef __cplusplus
}
#endif
