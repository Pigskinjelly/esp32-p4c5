/*
 * 内置 App 注册表
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 依次注册：监控 / 相机 / 相册 / 股票 / 天气 / 设置（在 ui_shell_start 之前调用） */
void ui_apps_register_all(void);

/**
 * 天气摘要（供锁屏显示）。有数据返回 true。
 * @param temp 当前温度  @param tmax/tmin 今日最高/最低  @param code WMO 天气码
 * @param city 城市名（静态字符串）
 */
bool ui_weather_summary(float *temp, float *tmax, float *tmin, int *code, const char **city);

/** 天气文字（如"晴"/"多云"），无数据返回空串 */
const char *ui_weather_desc(void);

/** 开机联网后启动天气自动获取（不进入 App 也有数据） */
void ui_weather_boot(void);

#ifdef __cplusplus
}
#endif
