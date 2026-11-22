/*
 * 桌面 Shell —— 手机式界面框架
 *
 * 结构：
 *   [状态栏 30px] 时间 / WiFi 图标
 *   [桌面]        App 图标网格
 *   [App 容器]    全屏，打开时盖在桌面上，顶栏有返回
 *   [控制中心]    从顶部下拉：WiFi 列表 / 亮度 / 状态
 *
 * 相机帧：video 任务把 PPA 输出写进 Shell 预览缓冲，
 * Shell 的 LVGL 定时器把最新帧刷给所有已注册的预览图控件。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHELL_PREVIEW_W     480
#define SHELL_PREVIEW_H     480
#define SHELL_PREVIEW_NUM   3

/** App 描述：桌面图标 + 创建/显示/隐藏回调 */
typedef struct {
    const char *name;                       /* 图标下方文字 */
    const char *symbol;                     /* LVGL 符号字符串 */
    uint32_t    color;                      /* 图标底色 */
    void (*create)(lv_obj_t *parent);       /* 首次打开时创建 UI（parent 480x480） */
    void (*on_show)(void);                  /* 可为 NULL */
    void (*on_hide)(void);                  /* 可为 NULL */
    bool        always_on;                  /* true = 不允许在设置里关闭（设置本身） */
} ui_app_t;

/** 初始化 LVGL + Shell（屏幕/触摸已由外部初始化） */
esp_err_t ui_shell_start(esp_lcd_panel_handle_t panel_handle, esp_lcd_touch_handle_t touch);

/** 注册 App（在 ui_shell_start 之前调用） */
void ui_shell_register_app(const ui_app_t *app);

/* ---- App 开关（设置里管理；关闭的 App 不显示图标、不创建 UI、不跑后台） ---- */
int         ui_shell_app_count(void);
const char *ui_shell_app_name(int idx);
bool        ui_shell_app_enabled(int idx);
/** 打开/关闭某个 App（自动存 NVS 并重建桌面图标） */
void        ui_shell_set_app_enabled(int idx, bool on);
/** 按名字查是否启用（供其它模块在开机时决定要不要起后台任务） */
bool        ui_shell_app_on_by_name(const char *name);
/** 后台运行开关：关闭时退出 App 会销毁其界面，释放 LVGL 内存 */
bool        ui_shell_app_bg(int idx);
void        ui_shell_app_bg_set(int idx, bool on);
/** 全局提醒卡片（任意任务可调用；显示在最上层，锁屏也看得到） */
void        ui_shell_alert_post(const char *title, const char *msg);
/** 按名字查“后台运行”开关 */
bool        ui_shell_app_bg_by_name(const char *name);
/** 正在销毁 App（on_hide 里可据此停掉播放等） */
bool        ui_shell_app_shutting(void);
/** 锁屏的时间/天气是否已就绪（开机自检等它） */
bool        ui_shell_lock_ready(void);

/** App 顶栏返回按钮的事件回调（ui_theme 使用） */
void ui_shell_back_event_cb(lv_event_t *e);

/** 调试用：按索引打开 App（0=监控 1=相机 2=相册 3=交易 4=音乐 5=天气 6=设置） */
void ui_shell_open_app(int idx);

/** 顶部浮动提示（1.6 秒自动消失，任意线程禁止——仅 LVGL 上下文调用） */
void ui_shell_toast(const char *txt);

/* ---- 相机预览帧管道 ---- */

/** 取第 idx 块预览缓冲（video 任务 PPA 直接往里写），大小 480*480*2 */
uint8_t *ui_shell_preview_buf(int idx);

/** video 任务：第 idx 块缓冲已完成（原子记录最新帧） */
void ui_shell_on_frame(int idx);

/** App 里创建一个自动跟随相机帧的图像控件 */
lv_obj_t *ui_shell_preview_attach(lv_obj_t *parent);

/** 当前最新预览帧的图像描述符（拍照用） */
const lv_image_dsc_t *ui_shell_preview_dsc(int idx);

/** 当前最新预览帧的图像描述符（自动取最新帧，无帧时 NULL） */
const lv_image_dsc_t *ui_shell_preview_latest_dsc(void);
/* 诊断用：当前显示的帧号（推进说明预览在刷新） */
int  ui_shell_preview_shown(void);

/* ---- 亮度 ---- */
void ui_shell_set_brightness(int pct);
int  ui_shell_get_brightness(void);

/* ---- 待机息屏（0 = 永不）：到点不是黑屏，是进锁屏界面 ---- */
void ui_shell_set_idle_timeout(int seconds);
int  ui_shell_get_idle_timeout(void);

/* ---- 屏幕显示方向（整个 UI 一起转，方屏旋转不损失分辨率） ---- */
typedef enum {
    UI_ROT_0 = 0,     /* 横向（默认） */
    UI_ROT_90 = 1,    /* 纵向（顺时针 90°） */
    UI_ROT_180 = 2,   /* 横向反转 */
    UI_ROT_270 = 3,   /* 纵向反转 */
} ui_rot_t;

void     ui_shell_set_rotation(ui_rot_t rot);
ui_rot_t ui_shell_get_rotation(void);
/** 相机画面需要跟着转的角度（0/90/180/270，逆时针，给 PPA 用） */
int      ui_shell_camera_rotation(void);

/* ---- 全屏手势开关（看图等场景要临时关掉，避免误退） ---- */
void ui_shell_set_gesture_enabled(bool on);

/* ---- 清空预览缓冲（切换拍照画幅时清掉旧的黑边/旧画面） ---- */
void ui_shell_preview_clear(void);

/* ---- wifi/其他事件通知（事件任务里调用，只置脏标志） ---- */
void ui_shell_wifi_notify(void *arg);

/** 由 shell 周期调用注册的 App tick（500ms，LVGL 上下文）——App 内部用 */
void ui_shell_set_tick_cb(void (*tick)(void));

#ifdef __cplusplus
}
#endif
