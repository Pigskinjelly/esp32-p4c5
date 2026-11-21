/*
 * 音频采集（ES8311 ADC + I2S0），用于远程监听。
 * 复用 esp_codec_dev 1.6.x 内置 ES8311 驱动（CONFIG_CODEC_ES8311_SUPPORT=y）。
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 ES8311 编解码器与 I2S 采集通道。
 * @param i2c_bus 共享 I2C 总线句柄（GPIO7=SDA, GPIO8=SCL）。
 */
esp_err_t audio_capture_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief 读取一帧 PCM 数据（16-bit 单声道）。
 * @return 实际读取字节数，<=0 表示无数据/错误。
 */
int audio_capture_read(void *buf, int len);

#ifdef __cplusplus
}
#endif
