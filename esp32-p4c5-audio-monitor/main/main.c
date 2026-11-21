/*
 * ESP32-P4C5 远程音频监控 demo 入口
 *
 * 链路：板载麦克风(LMA3729) -> ES8311 ADC -> I2S0 -> WebSocket 推流 -> 浏览器实时监听
 * 网络：ESP32-C5 通过 SDIO/esp_hosted 提供 Wi-Fi（esp_wifi_remote 透明转发标准 esp_wifi API）
 */
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/i2c_master.h"
#include "wifi_sta.h"
#include "audio_capture.h"
#include "audio_stream.h"

static const char *TAG = "main";

static i2c_master_bus_handle_t s_i2c_bus = NULL;

void app_main(void)
{
    /* 0. 基础网络/事件环境 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 1. 共享 I2C 总线（ES8311 使用；相机/触摸未接时仅 codec 一个设备） */
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = GPIO_NUM_8,
        .sda_io_num = GPIO_NUM_7,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &s_i2c_bus));
    ESP_LOGI(TAG, "shared I2C bus ready (SDA=7 SCL=8)");

    /* 2. 音频采集（ES8311 + I2S0） */
    ESP_ERROR_CHECK(audio_capture_init(s_i2c_bus));

    /* 3. Wi-Fi（esp_wifi_remote -> C5 via SDIO） */
    ESP_ERROR_CHECK(wifi_sta_start());

    /* 4. Web 服务 + 音频推流 */
    ESP_ERROR_CHECK(audio_stream_start());

    ESP_LOGI(TAG, "remote audio monitor running. open http://<board-ip>/ in browser");
}
