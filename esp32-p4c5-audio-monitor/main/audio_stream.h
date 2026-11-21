/*
 * Web 服务 + WebSocket 音频推流 + 前端页面。
 * 采集任务把麦克风 PCM 通过 WebSocket 广播给浏览器，浏览器用 Web Audio API 实时播放。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 HTTP 服务器与 WebSocket 音频推流任务。
 * @note 需在 Wi-Fi 就绪、音频采集初始化完成后调用。
 */
esp_err_t audio_stream_start(void);

#ifdef __cplusplus
}
#endif
