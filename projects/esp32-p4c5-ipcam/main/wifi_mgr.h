/*
 * Wi-Fi 管理器 —— 供桌面系统的控制中心调用
 *
 * 功能：
 *   - STA 初始化（经 C5 协处理器 esp_wifi_remote/esp_hosted）
 *   - 异步扫描周围热点，结果缓存供 UI 读取
 *   - 连接指定 SSID（带重试），凭据存 NVS，开机自动重连
 *   - 连接成功后启动 SNTP 对时（东八区）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_IDLE = 0,      /* 已启动，未在连接 */
    WIFI_MGR_SCANNING,      /* 扫描中 */
    WIFI_MGR_CONNECTING,    /* 连接中（含重试） */
    WIFI_MGR_CONNECTED,     /* 已拿到 IP */
} wifi_mgr_state_t;

typedef struct {
    char     ssid[33];
    int8_t   rssi;
    uint8_t  authmode;      /* wifi_auth_mode_t */
    uint8_t  channel;
} wifi_mgr_ap_info_t;

/* UI 传入的异步通知回调（事件上下文，回调里不要做 LVGL 操作，只置标志） */
typedef void (*wifi_mgr_notify_cb_t)(void *arg);

/**
 * @brief 初始化 Wi-Fi 子系统并尝试自动重连上次的热点（非阻塞）
 *
 * 首次开机使用 Kconfig 中配置的 SSID/密码作为默认凭据。
 */
esp_err_t wifi_mgr_init(void);

/** 注册状态变化通知（连接成功/断开/扫描完成均会触发） */
void wifi_mgr_set_notify_cb(wifi_mgr_notify_cb_t cb, void *arg);

/** 异步启动扫描；完成后通知回调触发，用 wifi_mgr_get_scan_results() 取结果 */
esp_err_t wifi_mgr_scan_async(void);

/** 强制重新扫描（忽略 60 秒缓存） */
esp_err_t wifi_mgr_scan_force(void);

/** 取最近一次扫描结果，返回条数（同名 SSID 已去重，保留信号最强的） */
int wifi_mgr_get_scan_results(wifi_mgr_ap_info_t *out, int max);

/** 查已保存的密码：返回 NULL 表示该网络没存过 */
const char *wifi_mgr_saved_pass(const char *ssid);

/** 该网络是否保存过密码 */
bool wifi_mgr_is_saved(const char *ssid);

/** 已保存（连接过）的网络数量 / 第 i 个的 SSID（控制中心展示用） */
int         wifi_mgr_saved_count(void);
const char *wifi_mgr_saved_ssid_at(int i);

/** 连接热点（会停止当前流程，保存凭据到 NVS，非阻塞） */
esp_err_t wifi_mgr_connect(const char *ssid, const char *password);

/** 断开（保留凭据，可再次自动重连） */
void wifi_mgr_disconnect(void);

/** WiFi 射频开关：关掉后停止 WiFi 省电，再打开会自动重连上次网络 */
bool      wifi_mgr_is_on(void);
esp_err_t wifi_mgr_set_on(bool on);

/** 忘记网络：断开并清除 NVS 凭据 */
void wifi_mgr_forget(void);

/** 忘记指定 SSID（用于"保存的网络"列表里的删除按钮） */
esp_err_t wifi_mgr_forget_ssid(const char *ssid);

wifi_mgr_state_t wifi_mgr_get_state(void);
const char      *wifi_mgr_get_ssid(void);   /* 当前/最近目标 SSID */
const char      *wifi_mgr_get_ip(void);     /* 未连接时 "0.0.0.0" */
int              wifi_mgr_get_rssi(void);   /* 未连接时 0 */
const char      *wifi_mgr_state_str(wifi_mgr_state_t st);

/** 最近一次连接失败是否是密码/认证问题（UI 用来提示"密码错误"） */
bool             wifi_mgr_auth_failed(void);

/** 最近一次断开原因（wifi_err_reason_t，-1 表示还没有过断开事件）。
 *  用于 UI 区分"密码错误 / 找不到 AP / 超时 / 被拒绝"，见 wifi_ui_stage()。 */
int              wifi_mgr_last_reason(void);

#ifdef __cplusplus
}
#endif
