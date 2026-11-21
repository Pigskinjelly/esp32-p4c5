/*
 * 音频采集实现（ES8311 + I2S0）。
 * 硬件（V1.5 底板）：
 *   ES8311 I2C 地址 0x18（7bit），8 位地址 0x30；与相机共用 I2C（GPIO7/8）。
 *   I2S0：GPIO9=DOUT、GPIO11=DIN(麦克风 ADC 输出)、GPIO10=WS、GPIO12=BCLK、GPIO13=MCLK。
 */
#include <string.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "audio_capture.h"

static const char *TAG = "audio_capture";

#define I2S_BCLK_IO         (GPIO_NUM_12)
#define I2S_WS_IO           (GPIO_NUM_10)
#define I2S_DOUT_IO         (GPIO_NUM_9)   /* P4 -> ES8311 DSDIN（采集时可不接，保留） */
#define I2S_DIN_IO          (GPIO_NUM_11)  /* ES8311 ASDOUT -> P4（麦克风数据输入） */
#define I2S_MCLK_IO         (GPIO_NUM_13)

static esp_codec_dev_handle_t s_codec_dev;

esp_err_t audio_capture_init(i2c_master_bus_handle_t i2c_bus)
{
    int sample_rate = CONFIG_MONITOR_AUDIO_SAMPLE_RATE;

    /* 1. I2S 通道（P4 为主，输出 MCLK 给 ES8311） */
    i2s_chan_handle_t i2s_tx = NULL;
    i2s_chan_handle_t i2s_rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, &i2s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = sample_rate,
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
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx, &std_cfg));

    /* 2. codec 控制接口（I2C，复用共享总线；8 位地址 0x30） */
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

    /* 3. ES8311 codec 接口（ADC+DAC，为后续对讲预留 DAC） */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = NULL,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = -1,
        .pa_reverted = false,
        .master_mode = false,   /* P4 是 I2S 主 */
        .use_mclk = true,
        .digital_mic = false,   /* 板上模拟麦 LMA3729 */
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3, .pa_gain = 0 },
        .no_dac_ref = false,
        .mclk_div = 0,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "failed to create ES8311 codec interface");
        return ESP_FAIL;
    }

    /* 4. codec 数据接口（I2S） */
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

    /* 5. 创建设备并打开 */
    esp_codec_dev_cfg_t dev_cfg = {
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_codec_dev == NULL) {
        ESP_LOGE(TAG, "failed to create codec device");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_codec_dev, &fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(s_codec_dev, 30.0)); /* 麦克风增益 dB */

    ESP_LOGI(TAG, "audio capture ready (%d Hz mono 16bit)", sample_rate);
    return ESP_OK;
}

int audio_capture_read(void *buf, int len)
{
    if (s_codec_dev == NULL) {
        return -1;
    }
    return esp_codec_dev_read(s_codec_dev, buf, len);
}
