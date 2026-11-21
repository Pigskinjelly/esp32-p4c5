/*
 * H.264 硬件编码器封装（/dev/video11，V4L2 M2M）
 *
 * 输入：YUV420 (I420) 平面格式，与 CSI/ISP 输出一致
 * 输出：Annex-B H.264 码流（IDR 帧自带 SPS/PPS）
 *
 * 零拷贝：CSI 采集缓冲的指针直接以 V4L2_MEMORY_USERPTR 方式喂给编码器，
 *         不在用户态复制原始帧。
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_ENC_MAX_SPS_LEN     64
#define APP_ENC_MAX_PPS_LEN     64

/* 编码输出的一帧码流 */
typedef struct {
    const uint8_t *data;
    size_t         len;
    bool           keyframe;
    uint64_t       pts_us;
} app_enc_frame_t;

/* 每编码出一帧回调一次；回调内不得阻塞过久，数据指针仅在回调期间有效 */
typedef void (*app_enc_out_cb_t)(const app_enc_frame_t *frame, void *ctx);

/**
 * @brief 初始化编码器
 *
 * @param width    图像宽（须与 CSI 输出一致，16 对齐）
 * @param height   图像高
 * @param fps      帧率，用于码控
 * @param bitrate  目标码率 bps（25000 ~ 2500000）
 * @param cb       码流输出回调，可为 NULL
 * @param ctx      回调上下文
 */
esp_err_t app_encoder_init(int width, int height, int fps, int bitrate,
                           app_enc_out_cb_t cb, void *ctx);

/**
 * @brief 编码一帧 YUV420
 *
 * 同步阻塞直到编码完成，返回时回调已执行。
 *
 * @param yuv420  YUV420 平面数据（I420：Y 平面 + U 平面 + V 平面）
 * @param len     数据长度，应为 w*h*3/2
 * @param pts_us  采集时间戳（微秒）
 */
esp_err_t app_encoder_encode(const uint8_t *yuv420, size_t len, uint64_t pts_us);

/** @brief 运行时调整码率 */
esp_err_t app_encoder_set_bitrate(int bitrate);

/** @brief 取 SPS（裸 NAL，不含起始码），供 RTSP SDP 用 */
bool app_encoder_get_sps(const uint8_t **data, size_t *len);

/** @brief 取 PPS（裸 NAL，不含起始码） */
bool app_encoder_get_pps(const uint8_t **data, size_t *len);

/** @brief 已编码帧计数与统计 */
void app_encoder_get_stats(uint32_t *frames, uint32_t *keyframes, uint32_t *last_size);

void app_encoder_deinit(void);

#ifdef __cplusplus
}
#endif
