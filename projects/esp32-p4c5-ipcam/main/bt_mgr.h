/*
 * 蓝牙（BLE）开关
 *
 * 本板没有板载 BT 射频——蓝牙跑在 C5 协处理器上，通过 esp_hosted 的
 * VHCI(HCI over SDIO) 通道给 P4 的 NimBLE 主机栈用。
 *
 * 起停是「异步」的：bt_mgr_set() 只把请求投递给内部 worker，立即返回；
 * 真正的初始化/反初始化在 worker 任务里串行执行（几十 ms 到几秒）。
 * UI 用 bt_mgr_state() 轮询状态刷新开关，不要假设 set() 返回时已经生效。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BT_STATE_OFF = 0,   /* 已关闭 */
    BT_STATE_STARTING,  /* 正在打开（controller + host 初始化中） */
    BT_STATE_ON,        /* 已开启并完成 HCI 同步 */
    BT_STATE_STOPPING,  /* 正在关闭 */
    BT_STATE_ERROR,     /* 上次操作失败，栈已回滚到关闭 */
} bt_state_t;

/** 当前蓝牙是否已完全开启（等价于 state == BT_STATE_ON） */
bool bt_mgr_is_on(void);

/** 当前状态（可在任意任务里调用） */
bt_state_t bt_mgr_state(void);

/** 状态枚举转字符串，用于界面/日志 */
const char *bt_mgr_state_str(bt_state_t st);

/** 上一次失败的错误名；没有失败返回 "none" */
const char *bt_mgr_last_error_str(void);

/**
 * 请求开/关蓝牙（非阻塞）。
 * 返回 ESP_OK 表示请求已受理；ESP_ERR_INVALID_STATE 表示正在切换中。
 */
esp_err_t bt_mgr_set(bool on);

/** 蓝牙是否可用（固件是否编译了 BT 栈） */
bool bt_mgr_available(void);

/**
 * 手机位置回传回调：手机连上来往 RX 特征写 "纬度,经度" 即可（GCJ-02/WGS84 都行，
 * App 不做强制，地图端按 WGS84 处理）。用于没有 GPS 模块时用手机定位导航。
 */
typedef void (*bt_loc_cb_t)(double lat, double lon);
void bt_mgr_set_loc_cb(bt_loc_cb_t cb);

#ifdef __cplusplus
}
#endif
