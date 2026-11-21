/* 开机自检：转圈动画 + 逐项检查（显示 / 触摸 / 系统 / 内存 / 网络 / 相机 / 存储） */
#pragma once

#include <stdbool.h>

enum {
    BOOT_SYS = 0,
    BOOT_DISP,
    BOOT_TOUCH,
    BOOT_MEM,
    BOOT_WIFI,
    BOOT_CAM,
    BOOT_STORE,
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
