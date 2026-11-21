/*
 * 轻量 RTSP 服务器（RFC 2326）+ RTP/H.264（RFC 6184）
 *
 * 支持：
 *   - OPTIONS / DESCRIBE / SETUP / PLAY / PAUSE / TEARDOWN / GET_PARAMETER
 *   - RTP over UDP（默认）与 RTP over RTSP/TCP（interleaved）两种传输
 *   - H.264 单 NAL 与 FU-A 分片
 *
 * P0 阶段仅服务单个客户端；多点访问留待后续扩展。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTSP_DEFAULT_PORT       554
#define RTSP_STREAM_PATH        "stream"

/** 启动 RTSP 服务（内部创建监听任务） */
esp_err_t rtsp_server_start(int port);

/**
 * @brief 送入一帧 H.264 码流（Annex-B）
 *
 * 数据会被拷贝进内部环形队列，调用方可立即复用缓冲。
 * 队列满时丢弃最旧的帧，保证实时性。
 */
esp_err_t rtsp_server_push_frame(const uint8_t *data, size_t len, bool keyframe, uint64_t pts_us);

/** 当前是否有客户端处于 PLAY 状态 */
bool rtsp_server_has_client(void);

/** 统计：已发送 RTP 包数、字节数、当前客户端数 */
void rtsp_server_get_stats(uint32_t *pkts, uint32_t *bytes, int *clients);

void rtsp_server_stop(void);

#ifdef __cplusplus
}
#endif
