/*
 * 照片存储 —— 挂载 flash 上的 storage 分区（FATFS）
 *
 * TF 卡（SDMMC）尚未安装：后续装卡后在这里扩展 SD 挂载并把保存路径切过去，
 * 上层只用 app_storage_save_photo() / app_storage_list_photos() 等接口。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_MOUNT_POINT  "/storage"

/** 挂载（TF 卡优先，失败回落 flash storage），幂等 */
bool app_storage_mount(void);

bool app_storage_ready(void);

/** 照片实际保存目录（"/sdcard/DCIM" 或 "/storage"） */
const char *app_storage_root(void);

/** 当前是否落在 TF 卡上 */
bool app_storage_is_sd(void);

/** 保存一张照片（JPEG 字节流），文件名自动生成 IMG_XXXX.JPG；返回文件名 */
bool app_storage_save_photo(const uint8_t *data, size_t len, char *name_out, size_t name_sz);

/** 列出照片文件名（按名字升序），返回条数 */
int app_storage_list_photos(char names[][32], int max);

/** 读取一张照片到缓冲（缓冲由调用方分配） */
int app_storage_read_photo(const char *name, uint8_t *buf, size_t buf_sz);

/** 照片字节数；失败返回 -1 */
int app_storage_photo_size(const char *name);

/** 删除照片 */
bool app_storage_delete_photo(const char *name);

/** 手动格式化 flash storage（TF 卡在位时拒绝执行）。R-09 配套 */
esp_err_t app_storage_format(void);

/** 统计：已用 KB / 剩余 KB */
void app_storage_usage(int *used_kb, int *free_kb);

#ifdef __cplusplus
}
#endif
