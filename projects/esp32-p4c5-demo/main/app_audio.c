/*
 * ESP32-P4C5 整机 Demo — 音频实现（ES8311 编解码 + NS4150B 功放 + 麦克风回环）
 *
 * 基于 esp_codec_dev 1.6.x（内置 ES8311 驱动，CONFIG_CODEC_ES8311_SUPPORT=y）。
 * 硬件连接（V1.5 底板）：
 *   ES8311 I2C 地址 0x18（7bit，CE 经 10K 下拉），与相机共用 I2C（GPIO7=SDA, GPIO8=SCL）
 *   I2S0：GPIO9=DOUT(→codec DSDIN)、GPIO11=DIN(←codec ASDOUT)、GPIO10=WS、GPIO12=BCLK、GPIO13=MCLK
 *   功放 NS4150B STD：GPIO53，高电平使能（本组件直接控制，不走 codec 的 pa_pin）
 *   麦克风：板上 LMA3729 模拟麦，接入 ES8311 MIC1P/MIC1N
 */
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

static const char *TAG = "app_audio";

/* I2S0 引脚（对应 V1.5 底板 ES8311 连接） */
#define I2S_BCLK_IO         (GPIO_NUM_12)
#define I2S_WS_IO           (GPIO_NUM_10)
#define I2S_DOUT_IO         (GPIO_NUM_9)   /* P4 -> ES8311 DSDIN */
#define I2S_DIN_IO          (GPIO_NUM_11)  /* ES8311 ASDOUT -> P4 */
#define I2S_MCLK_IO         (GPIO_NUM_13)

/* NS4150B 功放使能（STD 高有效） */
#define PA_CTRL_IO          (GPIO_NUM_53)

#define SAMPLE_RATE         (16000)

static esp_codec_dev_handle_t s_codec_dev;

esp_err_t app_audio_init(i2c_master_bus_handle_t i2c_bus)
{
    /* 1. 功放使能（NS4150B STD 拉高；codec 侧 pa_pin=-1 不干预） */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << PA_CTRL_IO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pa_cfg));
    gpio_set_level(PA_CTRL_IO, 1);
    ESP_LOGI(TAG, "PA enabled (GPIO%d)", PA_CTRL_IO);

    /* 2. I2S 通道（P4 为主，输出 MCLK 给 ES8311） */
    i2s_chan_handle_t i2s_tx = NULL;
    i2s_chan_handle_t i2s_rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, &i2s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_IO,
            .bclk = I2S_BCLK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DOUT_IO,
            .din = I2S_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx, &std_cfg));

    /* 3. codec 控制接口（I2C，复用共享总线；addr 为 8 位地址 0x30） */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,  /* 0x30 = 0x18 << 1 */
        .bus_handle = i2c_bus,
        .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "failed to create codec i2c ctrl interface");
        return ESP_FAIL;
    }

    /* 4. ES8311 codec 接口 */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = NULL,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,   /* 回环：ADC + DAC 全双工 */
        .pa_pin = -1,          /* 功放由本组件直接控制 */
        .pa_reverted = false,
        .master_mode = false,  /* P4 是 I2S 主，ES8311 为从 */
        .use_mclk = true,
        .digital_mic = false,  /* 板上为模拟麦克风 LMA3729 */
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
            .pa_gain = 0,
        },
        .no_dac_ref = false,
        .mclk_div = 0,         /* 默认 256 分频 */
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "failed to create ES8311 codec interface");
        return ESP_FAIL;
    }

    /* 5. codec 数据接口（I2S） */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = i2s_rx,
        .tx_handle = i2s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "failed to create codec i2s data interface");
        return ESP_FAIL;
    }

    /* 6. 创建 codec 设备 */
    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_codec_dev == NULL) {
        ESP_LOGE(TAG, "failed to create codec device");
        return ESP_FAIL;
    }

    /* 7. 打开 codec */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = 0,   /* 256 */
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_codec_dev, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_codec_dev, 60));   /* 0-100 */
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(s_codec_dev, 30.0)); /* dB */

    ESP_LOGI(TAG, "ES8311 codec initialized (16kHz mono 16bit)");
    return ESP_OK;
}

void app_audio_loopback_task(void *arg)
{
    /* 约 20ms 的 16bit 单声道数据 */
    const size_t frame_bytes = SAMPLE_RATE * 2 * 20 / 1000; /* 640B */
    uint8_t *buf = calloc(1, frame_bytes);
    if (buf == NULL) {
        ESP_LOGE(TAG, "failed to alloc loopback buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "audio loopback task started");
    while (1) {
        int r = esp_codec_dev_read(s_codec_dev, buf, frame_bytes);
        if (r > 0) {
            esp_codec_dev_write(s_codec_dev, buf, r);
        }
    }
}
