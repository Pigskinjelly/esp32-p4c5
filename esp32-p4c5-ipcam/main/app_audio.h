/*
 * ESP32-P4C5 整机 Demo — 音频组件（ES8311 编解码 + NS4150B 功放 + 麦克风回环）
 *
 * 硬件连接（V1.5 底板）：
 *   ES8311 I2C 地址 0x18（CE 经 10K 下拉），与相机共用 I2C（GPIO7=SDA, GPIO8=SCL）
 *   I2S0：GPIO9=DOUT(→codec DSDIN)、GPIO11=DIN(←codec ASDOUT)、GPIO10=WS、GPIO12=BCLK、GPIO13=MCLK
 *   功放 NS4150B STD：GPIO53，高电平使能
 *   麦克风：板上 LMA3729（模拟麦），接入 ES8311 MIC1P/MIC1N
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化音频链路（I2S + ES8311 + 功放）。
 * @param i2c_bus 共享的 I2C 总线句柄（与相机 SCCB 共用 GPIO7/8）。
 */
esp_err_t app_audio_init(i2c_master_bus_handle_t i2c_bus);

/**
 * @brief 麦克风 → 喇叭 实时回环任务（esp_codec_dev_read → write）。
 */
void app_audio_loopback_task(void *arg);

/* ---- 播放（音乐 App 用） ---- */

/** 按指定格式重开输出（音乐文件可能是 44.1k/48k、单/双声道） */
esp_err_t app_audio_out_format(int sample_rate, int channels);

/** 写一段 PCM 数据，返回实际写入字节数（<0 失败） */
int app_audio_out_write(const void *buf, size_t len);

/** 丢弃缓冲里还没播完的 PCM（切歌时用，避免上一首尾巴混进来） */
void app_audio_out_flush(void);

/** 音量 0-100 */
void app_audio_set_volume(int vol);

/** 当前音量 0-100 */
int app_audio_get_volume(void);

/** 是否初始化成功 */
bool app_audio_ready(void);

#ifdef __cplusplus
}
#endif
