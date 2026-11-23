/* 开机自检：5 项逐项检查 + 进度 + 底部状态条（见 ui_boot.c 顶部说明） */
#pragma once

#include <stdbool.h>

/* 自检项：对齐设计稿 12-self-test 的 5 行 */
enum {
    BOOT_CAM = 0,   /* 相机 SC2336（含显示/触摸链路） */
    BOOT_STORE,     /* TF 卡 / flash 存储 */
    BOOT_WIFI,      /* WiFi 模块 */
    BOOT_RTSP,      /* RTSP 服务 */
    BOOT_MEM,       /* 内存 */
    BOOT_ITEM_CNT
};

/** 建立自检浮层（必须在 LVGL 起来之后、并且持有 LVGL 锁时调用） */
void ui_boot_check_begin(void);

/** 标记某一项结果（可在任意任务调用，只写内存，UI 由定时器刷新） */
void ui_boot_check_mark(int item, bool ok, const char *text);

/** 标记为"暂未就绪"（例如 WiFi 还没连上）：不算失败，只显示灰字 */
void ui_boot_check_skip(int item, const char *text);

/** 自检结束：收尾后淡出浮层 */
void ui_boot_check_done(void);
