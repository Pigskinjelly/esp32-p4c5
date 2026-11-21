/*
 * Wi-Fi STA 连接（esp_wifi_remote 透明转发到 C5，通过 SDIO/esp_hosted）
 *
 * 对应用而言就是标准 ESP-IDF Wi-Fi API；esp_hosted 的 SDIO 传输由 esp_wifi_remote
 * 组件自动初始化，无需显式调用 esp_hosted 接口。
 */
#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化并启动 Wi-Fi STA 连接。
 * @note 调用前需已执行 esp_netif_init() 与 esp_event_loop_create_default()。
 */
esp_err_t wifi_sta_start(void);

/**
 * @brief 判断是否已获取 IP（Wi-Fi 就绪）。
 */
bool wifi_sta_is_connected(void);

#ifdef __cplusplus
}
#endif
