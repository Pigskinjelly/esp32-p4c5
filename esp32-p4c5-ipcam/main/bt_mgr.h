/*
 * 蓝牙（BLE）开关
 *
 * 本板没有板载 BT 射频——蓝牙跑在 C5 协处理器上，通过 esp_hosted 的
 * VHCI(HCI over SDIO) 通道给 P4 的 NimBLE 主机栈用。
 * 因此这里只做「初始化/使能 BT 控制器 + 启停 NimBLE 主机 + 广播设备名」，
 * 关闭时再逐层反初始化。
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 当前蓝牙是否已开启 */
bool bt_mgr_is_on(void);

/** 开/关蓝牙；返回 ESP_OK 表示状态切换成功 */
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
