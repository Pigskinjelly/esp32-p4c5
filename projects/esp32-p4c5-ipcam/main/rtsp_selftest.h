/*
 * RTSP 回环自测：板子连自己的 127.0.0.1:554，走一遍
 * OPTIONS -> DESCRIBE -> SETUP(TCP interleaved) -> PLAY，并统计收到的 RTP 包。
 *
 * 仅用于没有第二台设备可测时验证服务端是否真的出流，默认关闭
 * （menuconfig: IP Camera Network -> RTSP 回环自测）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动自测任务（延迟若干秒后执行一次，结果打到日志） */
esp_err_t rtsp_selftest_start(int port, int delay_ms);

#ifdef __cplusplus
}
#endif
