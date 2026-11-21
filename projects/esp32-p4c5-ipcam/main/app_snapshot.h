/*
 * 全分辨率快照 —— 拍照
 *
 * 流程（关键：PPA 转换不能放在相机帧回调里做）：
 *   1. app_snapshot_request()：请求一张
 *   2. 下一帧到达时，帧回调只做一次 memcpy，把 YUV420 原始帧拷到私有缓冲
 *      （1.4MB ≈ 2ms，远快于一帧间隔，不会被下一帧 DMA 覆盖）
 *   3. app_snapshot_build()：在拍照任务里慢慢做 PPA 转换（按画幅裁切），
 *      不受帧率限制，画质和颜色都正确
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 画幅比例（拍照） */
typedef enum {
    SNAP_AR_1_1 = 0,    /* 1:1 正方形（默认，与 1:1 屏幕匹配） */
    SNAP_AR_4_3 = 1,    /* 4:3 */
    SNAP_AR_16_9 = 2,   /* 16:9 */
} snap_ar_t;

/** 请求一张快照（下一个相机帧到来时抓取原始帧） */
void app_snapshot_request(void);

/** 原始帧是否已抓取到私有缓冲（可以开始 build） */
bool app_snapshot_ready(void);

/** 按当前画幅做 PPA 转换（RGB565）。0 = 成功，之后 get 可用 */
int app_snapshot_build(void);

/** 取快照数据（RGB565）与尺寸；未就绪返回 NULL */
const uint8_t *app_snapshot_get(int *w, int *h);

/** 处理完清除就绪标志，允许下一次请求 */
void app_snapshot_clear(void);

/** 设置/读取拍照画幅（存 NVS） */
void      app_snapshot_set_aspect(snap_ar_t ar);
snap_ar_t app_snapshot_get_aspect(void);

/** 画面方向补偿（0/90/180/270，补偿摄像头在结构件里装反/装歪），掉电保存 */
int  app_snapshot_rot_off(void);
void app_snapshot_set_rot_off(int deg);
const char *app_snapshot_aspect_name(snap_ar_t ar);

/** 竖屏拍照（画幅宽高对调，如 16:9 → 9:16），存 NVS */
void app_snapshot_set_portrait(bool on);
bool app_snapshot_is_portrait(void);

#ifdef __cplusplus
}
#endif
