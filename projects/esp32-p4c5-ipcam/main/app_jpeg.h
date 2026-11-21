/*
 * JPEG 拍照 —— P4 硬件 JPEG 编码器（V4L2 M2M，/dev/video10）
 *
 * 一次拍照：把一帧 YUV420 写入 OUTPUT mmap 缓冲 → STREAMON → DQBUF 取码流。
 * 设备保持打开，拍一张编码一张（硬件，百毫秒级）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 打开 JPEG 编码设备并按输入尺寸配置（幂等；尺寸变化会重配） */
esp_err_t app_jpeg_init(int width, int height);

/** 编码一帧 RGB565（源须为 width*height*2 字节）。成功返回 JPEG 长度，失败 -1。
 *  out 缓冲由内部持有，下一次调用前有效。 */
int app_jpeg_encode_rgb565(const uint8_t *rgb565, size_t len, const uint8_t **out);

void app_jpeg_deinit(void);

#ifdef __cplusplus
}
#endif
