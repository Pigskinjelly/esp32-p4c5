/*
 * 桌面 Shell 实现
 */
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/param.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/ledc.h"
#include "nvs.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "ui_shell.h"
#include "ui_theme.h"
#include "ui_boot.h"
#include "ui_apps.h"
#include "wifi_mgr.h"
#include "bt_mgr.h"
#include "app_lcd.h"
#include "esp_wifi.h"

#define TAG "ui_shell"

/* ================= 预览帧管道 ================= */

static lv_image_dsc_t s_dsc[SHELL_PREVIEW_NUM];
static uint8_t *s_pbuf[SHELL_PREVIEW_NUM];
static volatile int s_frame_latest = -1;
static int s_frame_shown = -1;

typedef struct {
    lv_obj_t *img;
} preview_listener_t;

static preview_listener_t s_listeners[4];
#define SHELL_PREVIEW_LISTENERS 4
static int s_listener_cnt;

/* ================= App 框架 ================= */

#define MAX_APPS 8
static const ui_app_t *s_apps[MAX_APPS];
static int s_app_cnt;
static lv_obj_t *s_app_container[MAX_APPS];
static bool s_app_created[MAX_APPS];
static int s_app_active = -1;
static uint32_t s_app_off;      /* 位图：1 = 该 App 被关闭（NVS 持久化） */
static uint32_t s_app_bg;       /* 位图：1 = 允许后台（退出后保留界面/任务） */
static bool s_app_shutting;     /* 正在销毁 App：on_hide 里可据此收尾 */

static lv_obj_t *s_desktop;
static lv_obj_t *s_pages;      /* 桌面分页容器 */
static void (*s_tick_cb)(void);

/* ================= 锁屏控件 ================= */
static lv_obj_t *s_lockscr;
static lv_obj_t *s_lock_time;
static lv_obj_t *s_lock_date;
static lv_obj_t *s_lock_week;
static lv_obj_t *s_lock_wx;
static lv_obj_t *s_lock_city;



/* ================= 控件引用 ================= */

static lv_obj_t *s_statusbar_time;
static lv_obj_t *s_statusbar_wifi;
static lv_obj_t *s_statusbar_ssid;   /* 居中显示当前 SSID */
static lv_obj_t *s_cc;              /* 控制中心面板（layer_top） */
static lv_obj_t *s_cc_time;
static lv_obj_t *s_cc_date;
static lv_obj_t *s_cc_wifi_state;
static bool s_sw_sync;      /* 程序化改开关状态时抑制回调 */
static void cc_refresh(void);
static void ap_list_rebuild(void);
static void cc_open(void);
static void cc_close(void);
static void lock_click_cb(lv_event_t *e);
static void shell_timer_cb(lv_timer_t *t);
static lv_obj_t *s_cc_wifi_ip;
static lv_obj_t *s_cc_ap_list;
static lv_obj_t *s_cc_brightness;
static lv_obj_t *s_cc_bright_val;   /* 亮度数值 */
static lv_obj_t *s_cc_bt_sw;        /* 蓝牙开关 */
static lv_obj_t *s_cc_rf_sw;        /* WiFi 射频开关（与 WiFi 卡同步） */
static lv_obj_t *s_scan_modal;      /* 扫描 WiFi 弹窗 */
static bool s_wifi_dirty = true;
static bool s_ap_list_dirty;

/* 密码弹窗 */
static lv_obj_t *s_pw_modal;
static lv_obj_t *s_pw_ta;
static char s_pw_ssid[33];

/* 前置声明（手势回调里会用到） */
static void do_lock(void);
static void do_lock_ex(bool force);
/* 解锁：粒子消散（比直接淡出自然） */
static void particle_exec(void *obj, int32_t v)
{
    lv_obj_t *o = (lv_obj_t *)obj;
    int32_t *d = (int32_t *)lv_obj_get_user_data(o);
    if (!d) {
        return;
    }
    lv_obj_set_style_translate_x(o, d[0] * v / 100, 0);
    lv_obj_set_style_translate_y(o, d[1] * v / 100, 0);
    lv_obj_set_style_opa(o, (lv_opa_t)(255 - 255 * v / 100), 0);
}

static void particle_done(lv_anim_t *a)
{
    lv_obj_t *o = (lv_obj_t *)a->var;
    int32_t *d = (int32_t *)lv_obj_get_user_data(o);
    if (d) {
        lv_free(d);
    }
    lv_obj_delete(o);
}

static void spawn_dissolve(void)
{
    static bool seeded;
    if (!seeded) {
        seeded = true;
        srand((unsigned)esp_timer_get_time());
    }
    lv_obj_t *top = lv_layer_top();
    static const uint32_t cols[4] = { 0xFFFFFF, 0x8FB6FF, 0xA99BFF, 0xD9E4FF };
    for (int i = 0; i < 40; i++) {
        lv_obj_t *p = lv_obj_create(top);
        int sz = 4 + rand() % 7;
        lv_obj_set_size(p, sz, sz);
        lv_obj_set_pos(p, rand() % SHELL_PREVIEW_W, rand() % SHELL_PREVIEW_H);
        lv_obj_set_style_bg_color(p, col(cols[i & 3]), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_set_style_pad_all(p, 0, 0);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);

        int32_t *d = lv_malloc(sizeof(int32_t) * 2);
        if (!d) {
            lv_obj_delete(p);
            continue;
        }
        d[0] = (rand() % 160) - 80;
        d[1] = (rand() % 160) - 80;
        lv_obj_set_user_data(p, d);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, p);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_time(&a, 360 + rand() % 220);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, particle_exec);
        lv_anim_set_completed_cb(&a, particle_done);
        lv_anim_start(&a);
    }
}

static void do_unlock(void);
static void global_gesture_cb(lv_event_t *e);

/** 递归给「子控件」开启冒泡：让子控件上产生的手势能上交给容器处理。
 *
 * 注意 LVGL 的手势事件不走普通事件冒泡（EVENT_BUBBLE），而是沿着
 * LV_OBJ_FLAG_GESTURE_BUBBLE 标记一路往父级找"第一个没有该标记的对象"。
 * 所以这里只给子对象打标记，容器本身不打 —— 这样手势正好停在容器上，
 * 由容器的 global_gesture_cb 处理。
 */
static void enable_bubble_recursive(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(obj, i);
        lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_gesture_bubble(c, true);   /* 手势专用的冒泡开关 */
        enable_bubble_recursive(c);
    }
}

/* ================= 亮度 / 待机息屏 ================= */

#define BL_PIN          20
#define IDLE_DEFAULT_S  60      /* 默认 60 秒无操作息屏 */
#define LOCK_BRIGHTNESS 22      /* 锁屏时降低背光省电 */
#define GESTURE_EDGE    34      /* 边缘手势区宽度（像素） */
static int s_brightness = 80;

static int      s_idle_timeout_s = IDLE_DEFAULT_S;   /* 0 = 永不 */
static int64_t  s_last_active_ms;
static bool     s_sleeping;
static bool     s_locked;           /* 锁屏（时间待机界面） */
static lv_display_t *s_disp;        /* LVGL 显示（旋转用） */
static ui_rot_t  s_rotation = UI_ROT_0;
static bool      s_gesture_enabled = true;
static int64_t   s_lock_ts;      /* 上次锁/解锁时间，防抖动 */

/* ================= 交互动画 ================= */

static void anim_y_exec(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

static void anim_scale_exec(void *obj, int32_t v)
{
    lv_obj_set_style_transform_scale((lv_obj_t *)obj, (int32_t)v, 0);
}

static void anim_opa_exec(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void anim_hide_done(lv_anim_t *a)
{
    lv_obj_t *o = (lv_obj_t *)a->user_data;
    if (o) {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 通用：垂直滑入/滑出（dur 默认 220ms，带缓动） */
static void slide_y(lv_obj_t *obj, int32_t from, int32_t to, uint32_t dur, bool hide_after)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_y_exec);
    if (hide_after) {
        a.user_data = obj;
        lv_anim_set_completed_cb(&a, anim_hide_done);
    }
    lv_anim_start(&a);
}

/* 全局按压反馈：只有「按钮」按下缩小到 92%，松开弹回。
 * 关键：必须记住按下的是哪个按钮 —— 松手时只能用同一个对象还原，
 * 否则会给卡片/图片/容器也设上 transform_scale，LVGL 一旦对对象做 transform，
 * 命中区域和手势判定就会失效（这就是全屏手势失灵的原因）。 */
static lv_obj_t *s_pressed_btn;

static void press_scale(lv_obj_t *obj, int32_t from, int32_t to, uint32_t dur)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, dur);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_scale_exec);
    lv_anim_start(&a);
}

static lv_point_t s_press_pt;      /* 本次按下的起点（手势边缘判定用） */
static bool       s_press_valid;

static void press_feedback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_indev_active();
        if (indev) {
            lv_indev_get_point(indev, &s_press_pt);
            s_press_valid = true;
        }
        lv_obj_t *obj = lv_indev_get_active_obj();
        if (!obj || !lv_obj_check_type(obj, &lv_button_class) ||
            lv_obj_has_state(obj, LV_STATE_DISABLED)) {
            s_pressed_btn = NULL;
            return;
        }
        s_pressed_btn = obj;
        press_scale(obj, 256, 236, 90);       /* 92% */
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CLICKED) {
        lv_obj_t *obj = s_pressed_btn;
        s_pressed_btn = NULL;
        if (obj && lv_obj_is_valid(obj)) {
            press_scale(obj, 236, 256, 140);
        }
    }
}
static int64_t  s_boot_ms;          /* 启动时刻，用于忽略开机瞬间的触摸脏数据 */
static lv_obj_t *s_sleep_overlay;

static void backlight_duty(uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
}

void ui_shell_set_brightness(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    s_brightness = pct;
    if (!s_sleeping) {
        backlight_duty((uint32_t)(s_locked ? LOCK_BRIGHTNESS : pct) * 1023 / 100);
    }
}

int ui_shell_get_brightness(void) { return s_brightness; }

/* ---- 待机时间（存 NVS，重启保留） ---- */
static void idle_load(void)
{
    nvs_handle_t h;
    int32_t v;
    if (nvs_open("sys", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "idle_s", &v) == ESP_OK && v >= 0 && v <= 3600) {
            s_idle_timeout_s = v;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "idle timeout: %d s (0=never)", s_idle_timeout_s);
}

void ui_shell_set_idle_timeout(int seconds)
{
    if (seconds < 0) seconds = 0;
    if (seconds > 3600) seconds = 3600;
    s_idle_timeout_s = seconds;
    s_last_active_ms = esp_timer_get_time() / 1000;
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "idle_s", seconds);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "idle timeout set to %d s", seconds);
}

int ui_shell_get_idle_timeout(void) { return s_idle_timeout_s; }

/* 待机（无操作超时）：进锁屏界面，而不是直接黑屏 */
static void do_sleep(void)
{
    if (s_locked) {
        return;
    }
    s_sleeping = false;          /* 不再使用黑屏遮罩 */
    do_lock_ex(true);
    ESP_LOGI(TAG, "idle %d s -> lock screen", s_idle_timeout_s);
}

static void do_wake(void)
{
    if (!s_sleeping) {
        return;
    }
    s_sleeping = false;
    s_last_active_ms = esp_timer_get_time() / 1000;
    backlight_duty((uint32_t)(s_locked ? LOCK_BRIGHTNESS : s_brightness) * 1023 / 100);
    if (s_sleep_overlay) {
        lv_obj_add_flag(s_sleep_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    ESP_LOGI(TAG, "screen on");
}

/* 息屏时第一下触摸只唤醒，不触发下层按钮（和手机一致） */
static void sleep_overlay_cb(lv_event_t *e)
{
    (void)e;
    do_wake();
}

/* 每 30ms 检查一次：有触摸就刷新活跃时间，超时则息屏 */
static void idle_check(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    lv_indev_t *indev = lv_indev_get_next(NULL);
    bool pressed = false;
    while (indev) {
        if (lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED) {
            pressed = true;
            break;
        }
        indev = lv_indev_get_next(indev);
    }
    if (pressed) {
        s_last_active_ms = now;
        if (s_sleeping) {
            do_wake();
        }
        return;
    }
    if (s_sleeping) {
        return;
    }
    /* 用户手动锁屏后只显示时间，不再自动熄屏（保持能看时间） */
    if (s_locked) {
        return;
    }
    if (s_idle_timeout_s > 0 &&
        (now - s_last_active_ms) > (int64_t)s_idle_timeout_s * 1000) {
        do_sleep();
    }
}

static void brightness_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_1,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);
    ledc_channel_config_t ch = {
        .gpio_num = BL_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_1,
        .duty = 819,
        .hpoint = 0,
    };
    ledc_channel_config(&ch);
}

/* ================= 小工具 ================= */

void ui_shell_back_event_cb(lv_event_t *e)
{
    (void)e;
    int idx = s_app_active;
    if (idx < 0) {
        return;
    }
    bool bg = ui_shell_app_bg(idx);

    /* 没开"后台"的 App：退出就销毁整套界面，把 LVGL 内存还回来。
     * 开着的只隐藏，下次进来还是原样（比如音乐接着放）。 */
    s_app_shutting = !bg;
    if (s_apps[idx]->on_hide) {
        s_apps[idx]->on_hide();
    }
    s_app_shutting = false;
    s_tick_cb = NULL;

    if (bg) {
        if (s_app_container[idx]) {
            lv_obj_add_flag(s_app_container[idx], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (s_app_container[idx]) {
            lv_obj_delete(s_app_container[idx]);
            s_app_container[idx] = NULL;
        }
        s_app_created[idx] = false;
        ESP_LOGI(TAG, "app[%d] \"%s\" closed, ui freed (%u KB free)",
                 idx, s_apps[idx]->name,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    }
    s_app_active = -1;
    lv_obj_remove_flag(s_desktop, LV_OBJ_FLAG_HIDDEN);
}

/* 去掉标题栏后，把 App 内容整体上移 34px（只动直接子对象，全屏背景不动） */
static void app_shift_up(lv_obj_t *root)
{
    uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(root, i);
        int y = lv_obj_get_y(c);
        if (y >= 34) {
            lv_obj_set_y(c, y - 34);
        }
    }
}

static void open_app_idx(int idx)
{
    if (idx < 0 || idx >= s_app_cnt) {
        return;
    }
    if (!ui_shell_app_enabled(idx)) {
        ui_shell_toast("该 App 已在设置里关闭");
        return;                 /* 关掉的 App 连代码都不进 */
    }
    /* 容器按需创建：关掉/没打开过的 App 不占 LVGL 内存 */
    if (!s_app_container[idx]) {
        lv_obj_t *c = lv_obj_create(lv_screen_active());
        lv_obj_set_size(c, 480, 480);
        lv_obj_set_pos(c, 0, 0);
        lv_obj_set_style_bg_color(c, col(C_BG), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(c, 0, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_set_style_radius(c, 0, 0);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(c, global_gesture_cb, LV_EVENT_GESTURE, NULL);
        s_app_container[idx] = c;
        s_app_created[idx] = false;
    }
    if (!s_app_created[idx]) {
        s_apps[idx]->create(s_app_container[idx]);
        app_shift_up(s_app_container[idx]);
        s_app_created[idx] = true;
    }
    lv_obj_add_flag(s_desktop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_app_container[idx], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_app_container[idx]);
    /* App 内的子控件也要把手势冒泡上来，否则在按钮/卡片上滑不动 */
    enable_bubble_recursive(s_app_container[idx]);
    if (s_apps[idx]->on_show) {
        s_apps[idx]->on_show();
    }
    s_app_active = idx;
}

static void desktop_app_click_cb(lv_event_t *e)
{
    open_app_idx((int)(intptr_t)lv_event_get_user_data(e));
}

/* 调试用：按索引打开 App（内部加 LVGL 锁，任意任务可调用） */
void ui_shell_open_app(int idx)
{
    lvgl_port_lock(0);
    open_app_idx(idx);
    lvgl_port_unlock();
}

/* Toast：顶部浮动提示，1.6s 后消失 */
static lv_timer_t *s_toast_timer;
static lv_obj_t *s_toast_label;

static void toast_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_toast_label) {
        lv_obj_delete(s_toast_label);
        s_toast_label = NULL;
    }
    lv_timer_del(s_toast_timer);
    s_toast_timer = NULL;
}

void ui_shell_toast(const char *txt)
{
    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (!s_toast_label) {
        s_toast_label = lv_label_create(lv_layer_top());
        lv_obj_set_style_bg_color(s_toast_label, col(0x000000), 0);
        lv_obj_set_style_bg_opa(s_toast_label, LV_OPA_70, 0);
        lv_obj_set_style_pad_all(s_toast_label, 8, 0);
        lv_obj_set_style_radius(s_toast_label, 8, 0);
    }
    lv_label_set_text(s_toast_label, txt);
    lv_obj_set_style_text_font(s_toast_label, &ui_cjk_16, 0);
    lv_obj_set_style_text_color(s_toast_label, col(0xFFFFFF), 0);
    lv_obj_center(s_toast_label);
    lv_obj_move_foreground(s_toast_label);
    s_toast_timer = lv_timer_create(toast_timer_cb, 1600, NULL);
}

/* ================= 预览帧 ================= */

uint8_t *ui_shell_preview_buf(int idx)
{
    if (idx < 0 || idx >= SHELL_PREVIEW_NUM) {
        return NULL;
    }
    return s_pbuf[idx];
}

/* 诊断用：当前显示的是第几号缓冲（能判断画面是否在推进） */
int ui_shell_preview_shown(void)
{
    return s_frame_shown;
}

void ui_shell_on_frame(int idx)
{
    s_frame_latest = idx;
}

/* ================= 全局提醒（价格提醒等） ================= */
static char s_alert_title[48];
static char s_alert_msg[96];
static volatile bool s_alert_pending;
static lv_obj_t *s_alert_card;
static int64_t s_alert_until;
/* 最近一次提醒的时间（锁屏右上角显示"xx 分钟前"，重启后仍保留） */
static int64_t s_last_alert_ms;


/* 距今多久：刚刚 / 20 分钟前 / 3 小时前 / 2 天前 */
static void rel_time_text(int64_t ms, char *buf, size_t cap)
{
    if (ms < 0) {
        ms = 0;
    }
    if (ms < 60 * 1000LL) {
        snprintf(buf, cap, "刚刚");
    } else if (ms < 3600 * 1000LL) {
        snprintf(buf, cap, "%d 分钟前", (int)(ms / (60 * 1000LL)));
    } else if (ms < 24 * 3600 * 1000LL) {
        snprintf(buf, cap, "%d 小时前", (int)(ms / (3600 * 1000LL)));
    } else {
        snprintf(buf, cap, "%d 天前", (int)(ms / (24 * 3600 * 1000LL)));
    }
}

void ui_shell_alert_post(const char *title, const char *msg)
{
    strlcpy(s_alert_title, title ? title : "提醒", sizeof(s_alert_title));
    strlcpy(s_alert_msg, msg ? msg : "", sizeof(s_alert_msg));
    s_alert_pending = true;

    time_t sec = time(NULL);
    s_last_alert_ms = (sec > 1700000000) ? (int64_t)sec * 1000 : 0;

}

static void alert_ok_cb(lv_event_t *e)
{
    (void)e;
    if (s_alert_card) {
        lv_obj_delete(s_alert_card);
        s_alert_card = NULL;
    }
}

/* 提醒卡片画在 layer_top：锁屏、任何 App 之上都能看到 */
static void alert_show_card(void)
{
    if (s_alert_card) {
        lv_obj_delete(s_alert_card);
        s_alert_card = NULL;
    }
    lv_obj_t *card = lv_obj_create(lv_layer_top());
    s_alert_card = card;
    lv_obj_set_size(card, 420, 150);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, col(0x1A2233), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, col(0xFFB060), 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = ui_label(card, s_alert_title, &ui_cjk_16, 0xFFD166);
    lv_obj_set_pos(t, 2, 0);
    lv_obj_t *m = ui_label(card, s_alert_msg, &ui_cjk_16, C_TXT);
    lv_obj_set_pos(m, 2, 34);
    lv_obj_set_width(m, 380);

    /* 右上角显示这条提醒的时间（几点触发的） */
    char tb[16] = "";
    time_t ts = (s_last_alert_ms > 0) ? (time_t)(s_last_alert_ms / 1000) : time(NULL);
    struct tm tt;
    localtime_r(&ts, &tt);
    if (tt.tm_year + 1900 >= 2024) {
        snprintf(tb, sizeof(tb), "%02d:%02d", tt.tm_hour, tt.tm_min);
    }
    lv_obj_t *ago = ui_label(card, tb, &lv_font_montserrat_16, 0x8FA8CC);
    lv_obj_align(ago, LV_ALIGN_TOP_RIGHT, 0, 2);

    lv_obj_t *btn = lv_button_create(card);
    lv_obj_set_size(btn, 104, 34);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn, col(0x2E4C7A), 0);
    lv_obj_set_style_radius(btn, 17, 0);
    lv_obj_add_event_cb(btn, alert_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(btn, "知道了", &ui_cjk_16, C_TXT);
    lv_obj_center(bl);

    lv_obj_move_foreground(card);
    s_alert_until = esp_timer_get_time() / 1000 + 10000;   /* 10 秒自动关 */
}

static void global_gesture_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE) {
        return;
    }
    /* 看图等场景临时关掉手势，避免拖动图片被误判成"返回" */
    if (!s_gesture_enabled) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_NONE) {
        return;
    }

    /* 1. 锁屏：任意滑动即解锁（不受边缘限制） */
    if (s_locked) {
        do_unlock();
        return;
    }

    /* 手机式边缘手势：只有从屏幕边缘滑起才算全局手势，
     * 屏幕中间滑动留给页面本身（翻页、拖动、列表滚动） */
    /* 用按下起点判断是否从边缘起手。
     * 不能用当前点：LVGL 要等手指移动约 50px 才判定手势，
     * 那时触点早就离开边缘区了（这就是边缘滑动没反应的根因）。 */
    lv_point_t pt = s_press_pt;
    if (!s_press_valid) {
        lv_indev_get_point(indev, &pt);
    }
    bool edge_left = (pt.x < GESTURE_EDGE);
    bool edge_right = (pt.x > SHELL_PREVIEW_W - GESTURE_EDGE);
    bool edge_bottom = (pt.y > SHELL_PREVIEW_H - GESTURE_EDGE);
    bool edge_top = (pt.y < GESTURE_EDGE + 30);

    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        if (!edge_left && !edge_right) {
            return;          /* 中间横滑：交给页面翻页 */
        }
    } else if (dir == LV_DIR_TOP) {
        if (!edge_bottom) {
            return;          /* 中间上滑：交给列表滚动 */
        }
    } else if (dir == LV_DIR_BOTTOM) {
        if (!edge_top) {
            return;          /* 中间下滑：交给列表 */
        }
    }

    /* 2. 控制中心打开时：忽略所有手势，只能点「点击收起」关掉
     *    （以前上滑会收起，容易和列表滚动打架，按要求去掉） */
    if (s_cc && !lv_obj_has_flag(s_cc, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }

    /* 3. App 内：上滑回桌面 / 左或右边缘滑返回 */
    if (s_app_active >= 0) {
        if (dir == LV_DIR_TOP || dir == LV_DIR_RIGHT || dir == LV_DIR_LEFT) {
            ui_shell_back_event_cb(NULL);
        }
        return;
    }

    /* 4. 桌面：下滑开控制中心 / 上滑锁屏 */
    if (dir == LV_DIR_BOTTOM) {
        cc_open();
    } else if (dir == LV_DIR_TOP) {
        do_lock();
    }
}

static void do_unlock(void)
{
    if (!s_locked) {
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (now_ms - s_lock_ts < 900) {      /* 防抖：切太快多半是触摸抖动 */
        return;
    }
    s_lock_ts = now_ms;
    s_locked = false;
    backlight_duty((uint32_t)s_brightness * 1023 / 100);
    if (s_lockscr) {
        spawn_dissolve();              /* 粒子向外飘散 */
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_lockscr);
        lv_anim_set_values(&a, 255, 0);
        lv_anim_set_time(&a, 160);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
        lv_anim_set_exec_cb(&a, anim_opa_exec);
        a.user_data = s_lockscr;
        lv_anim_set_completed_cb(&a, anim_hide_done);
        lv_anim_start(&a);
        lv_obj_set_style_opa(s_lockscr, 255, 0);
    }
    s_last_active_ms = esp_timer_get_time() / 1000;
    ESP_LOGI(TAG, "unlocked");
}

/* ================= 状态栏 / 控制中心 / 锁屏 ================= */
static void statusbar_update(void)
{
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    if (tm_info.tm_year + 1900 >= 2024) {
        lv_label_set_text_fmt(s_statusbar_time, "%02d:%02d",
                              tm_info.tm_hour, tm_info.tm_min);
        lv_label_set_text_fmt(s_cc_time, "%02d:%02d",
                              tm_info.tm_hour, tm_info.tm_min);
        static const char *wd[] = {"周日","周一","周二","周三","周四","周五","周六"};
        lv_label_set_text_fmt(s_cc_date, "%02d-%02d %s",
                              tm_info.tm_mon + 1, tm_info.tm_mday, wd[tm_info.tm_wday]);
    }
}
static void statusbar_wifi_update(void)
{
    wifi_mgr_state_t st = wifi_mgr_get_state();
    uint32_t color = (st == WIFI_MGR_CONNECTED) ? 0x37D8E8 :
                     (st == WIFI_MGR_CONNECTING || st == WIFI_MGR_SCANNING) ? 0xE8C537 : 0x5A6B8C;
    lv_obj_set_style_text_color(s_statusbar_wifi, lv_color_hex(color), 0);

    /* 居中显示当前 SSID（或状态） */
    const char *ssid = wifi_mgr_get_ssid();
    if (st == WIFI_MGR_CONNECTED && ssid[0] != '\0') {
        lv_label_set_text(s_statusbar_ssid, ui_font_safe(ssid, &ui_cjk_16));
    } else if (st == WIFI_MGR_CONNECTING) {
        lv_label_set_text(s_statusbar_ssid, "连接中…");
    } else {
        lv_label_set_text(s_statusbar_ssid, "未连接");
    }
    lv_obj_set_style_text_color(s_statusbar_ssid, lv_color_hex(color), 0);
}
static void statusbar_click_cb(lv_event_t *e)
{
    (void)e;
    cc_open();
}
/* 列表行数据：下标 → SSID（点行/忘记都用它，避免和扫描结果下标绑死） */
#define AP_ROWS 20
static char    s_row_ssid[AP_ROWS][33];
static uint8_t s_row_auth[AP_ROWS];
static int     s_row_cnt;

static void cc_ap_row_click_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_row_cnt) {
        return;
    }
    const char *ssid = s_row_ssid[idx];

    /* 已经连着这个网络：什么都不用做 */
    if (wifi_mgr_get_state() == WIFI_MGR_CONNECTED &&
        strcmp(wifi_mgr_get_ssid(), ssid) == 0) {
        ui_shell_toast("已连接到该网络");
        return;
    }
    if (s_row_auth[idx] == WIFI_AUTH_OPEN) {
        wifi_mgr_connect(ssid, "");
        ui_shell_toast("正在连接…");
        return;
    }
    /* 存过密码就直接连。
     * R-40：必须排除"保存的是空串"——之前存过空密码时 saved 指向 ""，
     * 结果为非 NULL，于是永远走这条路、永远不弹密码框，用户怎么点都连不上。 */
    const char *saved = wifi_mgr_saved_pass(ssid);
    if (saved != NULL && saved[0] != '\0') {
        wifi_mgr_connect(ssid, saved);
        ui_shell_toast("使用已保存的密码连接…");
        return;
    }
    /* 没存过才弹密码框 */
    strlcpy(s_pw_ssid, ssid, sizeof(s_pw_ssid));
    lv_obj_t *title = (lv_obj_t *)lv_obj_get_user_data(s_pw_modal);
    lv_label_set_text_fmt(title, "连接到 %s", ui_font_safe(ssid, &ui_cjk_16));
    lv_textarea_set_text(s_pw_ta, "");
    lv_obj_remove_flag(s_pw_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_pw_modal);
}

/* 行的「忘记」按钮：删掉保存的凭据，之后就不会再自动连它 */
static void cc_ap_forget_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_row_cnt) {
        return;
    }
    char ssid[33];
    strlcpy(ssid, s_row_ssid[idx], sizeof(ssid));
    if (wifi_mgr_forget_ssid(ssid) == ESP_OK) {
        ui_shell_toast("已忘记该网络，不会自动连接");
    } else {
        ui_shell_toast("忘记失败");
    }
    s_ap_list_dirty = true;
    cc_refresh();
}

/* 加一行；saved=true 时右侧给「忘记」按钮 */
static void ap_add_row(const char *ssid, int rssi, bool saved, bool connected)
{
    if (s_row_cnt >= AP_ROWS) {
        return;
    }
    int idx = s_row_cnt++;
    strlcpy(s_row_ssid[idx], ssid, sizeof(s_row_ssid[idx]));
    s_row_auth[idx] = 0;
    /* 从扫描结果里找 authmode */
    wifi_mgr_ap_info_t aps[16];
    int n = wifi_mgr_get_scan_results(aps, 16);
    for (int k = 0; k < n; k++) {
        if (strcmp(aps[k].ssid, ssid) == 0) {
            s_row_auth[idx] = aps[k].authmode;
            break;
        }
    }

    lv_obj_t *row = lv_button_create(s_cc_ap_list);
    lv_obj_set_size(row, 396, 46);
    lv_obj_set_style_bg_color(row, col(connected ? 0x123B36 : 0x1E2230), 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_event_cb(row, cc_ap_row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, ui_font_safe(ssid, &ui_cjk_16));
    lv_obj_set_style_text_font(name, &ui_cjk_16, 0);
    lv_obj_set_style_text_color(name, col(connected ? 0x22D3EE : C_TXT), 0);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 14, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, saved ? 180 : 230);

    /* 右端：忘记按钮（仅已保存） */
    if (saved) {
        lv_obj_t *fg = lv_button_create(row);
        lv_obj_set_size(fg, 56, 30);
        lv_obj_align(fg, LV_ALIGN_RIGHT_MID, -10, 0);
        lv_obj_set_style_bg_color(fg, col(0x332030), 0);
        lv_obj_set_style_radius(fg, 8, 0);
        lv_obj_set_style_pad_all(fg, 0, 0);
        lv_obj_add_event_cb(fg, cc_ap_forget_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
        lv_obj_t *fl = ui_label(fg, "忘记", &ui_cjk_16, 0xF09090);
        lv_obj_center(fl);
    }

    const char *tag = connected ? "已连接" : saved ? (rssi ? "已保存" : "未在附近") : NULL;
    if (tag) {
        lv_obj_t *t = ui_label(row, tag, &ui_cjk_16, connected ? 0x22D3EE : 0x7F8BA0);
        lv_obj_align(t, LV_ALIGN_RIGHT_MID, saved ? -78 : -70, 0);
    }
    if (rssi) {
        lv_obj_t *meta = lv_label_create(row);
        lv_label_set_text_fmt(meta, "%d", rssi);
        lv_obj_set_style_text_font(meta, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(meta, col(0x6B7488), 0);
        lv_obj_align(meta, LV_ALIGN_RIGHT_MID, saved ? -142 : -16, 0);
    }
}

static void ap_list_rebuild(void)
{
    lv_obj_clean(s_cc_ap_list);
    s_row_cnt = 0;

    wifi_mgr_ap_info_t aps[16];
    int n = wifi_mgr_get_scan_results(aps, 16);
    const char *cur = wifi_mgr_get_ssid();
    bool conn = (wifi_mgr_get_state() == WIFI_MGR_CONNECTED);

    /* 1) 先列"已保存"（含当前连接的），这样一定能在这里点「忘记」 */
    for (int i = 0; i < wifi_mgr_saved_count(); i++) {
        const char *ssid = wifi_mgr_saved_ssid_at(i);
        if (!ssid[0]) {
            continue;
        }
        int rssi = 0;
        for (int k = 0; k < n; k++) {
            if (strcmp(aps[k].ssid, ssid) == 0) {
                rssi = aps[k].rssi;
                break;
            }
        }
        ap_add_row(ssid, rssi, true, conn && strcmp(ssid, cur) == 0);
    }

    /* 2) 再列周围没保存过的 */
    for (int k = 0; k < n; k++) {
        if (wifi_mgr_is_saved(aps[k].ssid)) {
            continue;
        }
        ap_add_row(aps[k].ssid, aps[k].rssi, false, false);
    }

    if (s_row_cnt == 0) {
        lv_obj_t *empty = ui_label(s_cc_ap_list, "没扫到网络，点「重新扫描」再试", NULL, 0x6B7488);
        lv_obj_set_style_pad_left(empty, 6, 0);
    }
}

static void scan_btn_cb(lv_event_t *e)
{
    (void)e;
    esp_err_t err = wifi_mgr_scan_async();
    if (err != ESP_OK) {
        ui_shell_toast("扫描失败（连接过程中无法扫描）");
    } else {
        ui_shell_toast("扫描中…");
    }
}
static void cc_brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    ui_shell_set_brightness(v);
    if (s_cc_bright_val) {
        lv_label_set_text_fmt(s_cc_bright_val, "%d", v);
    }
}
static void wifi_switch_cb(lv_event_t *e)
{
    if (s_sw_sync) {
        return;
    }
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (wifi_mgr_set_on(on) != ESP_OK) {
        ui_shell_toast("WiFi 切换失败");
        if (on) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        return;
    }
    ui_shell_toast(on ? "WiFi 已开启，正在连接…" : "WiFi 已关闭（省电）");
    cc_refresh();
}
static void cc_close_bar_cb(lv_event_t *e)
{
    (void)e;
    cc_close();
}
static void cc_click_cb(lv_event_t *e)
{
    (void)e;
    cc_close();
}

static void cc_close(void)
{
    if (s_scan_modal) {
        lv_obj_add_flag(s_scan_modal, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_cc || lv_obj_has_flag(s_cc, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    slide_y(s_cc, lv_obj_get_y(s_cc), -SHELL_PREVIEW_H, 200, true);
}

static void pw_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_pw_modal, LV_OBJ_FLAG_HIDDEN);
}
static void pw_connect_cb(lv_event_t *e)
{
    (void)e;
    const char *pwd = lv_textarea_get_text(s_pw_ta);
    /* R-40：密码框能走到这里说明该网络是加密的（开放网络不弹框）。
     * 空密码提交会把 "" 写进 NVS 覆盖原密码，之后再也连不上。 */
    if (pwd[0] == '\0') {
        ui_shell_toast("请输入 WiFi 密码");
        return;
    }
    wifi_mgr_connect(s_pw_ssid, pwd);
    lv_obj_add_flag(s_pw_modal, LV_OBJ_FLAG_HIDDEN);
    ui_shell_toast("正在连接…");
}
/* ---- 控制中心小工具（对齐设计稿：深色卡片 + 青色开关） ---- */
static lv_obj_t *cc_card(lv_obj_t *parent, int y, int h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, 440, h);
    lv_obj_set_pos(c, 20, y);
    lv_obj_set_style_bg_color(c, col(0x14161E), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t *cc_switch(lv_obj_t *parent, int x, int y, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 54, 30);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_style_bg_color(sw, col(0x2A2F45), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, col(0x22D3EE), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, col(0xFFFFFF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 1, LV_PART_KNOB);
    if (on) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    if (cb) {
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    }
    return sw;
}

static void scan_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_scan_modal) {
        lv_obj_add_flag(s_scan_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scan_open_cb(lv_event_t *e)
{
    (void)e;
    if (!s_scan_modal) {
        return;
    }
    ap_list_rebuild();
    lv_obj_remove_flag(s_scan_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_scan_modal);
    esp_err_t err = wifi_mgr_scan_force();      /* 手动扫描：绕过 60 秒缓存 */
    if (err == ESP_OK) {
        ui_shell_toast("正在扫描附近 WiFi…");
    } else if (!wifi_mgr_is_on()) {
        ui_shell_toast("WiFi 已关闭，先打开 WiFi");
    } else {
        ui_shell_toast("扫描失败，稍后再试");
    }
}

static void bt_switch_cb(lv_event_t *e)
{
    if (s_sw_sync) {
        return;
    }
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (bt_mgr_set(on) != ESP_OK) {
        ui_shell_toast("蓝牙切换失败");
        s_sw_sync = true;
        if (on) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        s_sw_sync = false;
        return;
    }
    ui_shell_toast(on ? "蓝牙已开启" : "蓝牙已关闭");
}

/* 扫描弹窗：附近 WiFi 列表 */
static void build_scan_modal(void)
{
    s_scan_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_scan_modal, 480, 480);
    lv_obj_set_pos(s_scan_modal, 0, 0);
    lv_obj_set_style_bg_color(s_scan_modal, col(0x000000), 0);
    lv_obj_set_style_bg_opa(s_scan_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_scan_modal, 0, 0);
    lv_obj_set_style_radius(s_scan_modal, 0, 0);
    lv_obj_set_style_pad_all(s_scan_modal, 0, 0);
    lv_obj_clear_flag(s_scan_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_scan_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *panel = lv_obj_create(s_scan_modal);
    lv_obj_set_size(panel, 440, 388);
    lv_obj_set_pos(panel, 20, 46);
    lv_obj_set_style_bg_color(panel, col(0x12151E), 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = ui_label(panel, "附近的 WiFi", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(t, 16, 14);

    lv_obj_t *x = lv_button_create(panel);
    lv_obj_set_size(x, 34, 34);
    lv_obj_set_pos(x, 392, 8);
    lv_obj_set_style_radius(x, 17, 0);
    lv_obj_set_style_bg_color(x, col(0x1E2230), 0);
    lv_obj_set_style_pad_all(x, 0, 0);
    lv_obj_add_event_cb(x, scan_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = ui_label(x, "×", &ui_cjk_16, 0x9AA5B8);
    lv_obj_center(xl);

    s_cc_ap_list = lv_obj_create(panel);
    lv_obj_set_size(s_cc_ap_list, 408, 268);
    lv_obj_set_pos(s_cc_ap_list, 16, 52);
    lv_obj_set_style_bg_opa(s_cc_ap_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cc_ap_list, 0, 0);
    lv_obj_set_style_radius(s_cc_ap_list, 0, 0);
    lv_obj_set_style_pad_all(s_cc_ap_list, 0, 0);
    lv_obj_set_style_pad_bottom(s_cc_ap_list, 10, 0);
    lv_obj_set_style_pad_row(s_cc_ap_list, 8, 0);
    lv_obj_set_flex_flow(s_cc_ap_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_cc_ap_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_cc_ap_list, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *again = lv_button_create(panel);
    lv_obj_set_size(again, 180, 42);
    lv_obj_set_pos(again, 130, 330);
    lv_obj_set_style_radius(again, 12, 0);
    lv_obj_set_style_bg_color(again, col(0x8B5CF6), 0);
    lv_obj_add_event_cb(again, scan_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = ui_label(again, "重新扫描", &ui_cjk_16, C_TXT);
    lv_obj_center(al);
}

static void build_control_center(void)
{
    s_cc = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_cc, 480, 480);
    lv_obj_set_pos(s_cc, 0, 0);
    lv_obj_set_style_bg_color(s_cc, col(0x0B0F18), 0);
    lv_obj_set_style_bg_opa(s_cc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_cc, 0, 0);
    lv_obj_set_style_border_width(s_cc, 0, 0);
    lv_obj_set_style_pad_all(s_cc, 0, 0);
    lv_obj_add_flag(s_cc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_cc, global_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(s_cc, cc_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *t = ui_label(s_cc, "控制中心", &ui_cjk_24, C_TXT);
    lv_obj_set_pos(t, 20, 10);
    lv_obj_t *sub = ui_label(s_cc, "点击面板任意位置或底部按钮收起", &ui_cjk_16, 0x5F6B80);
    lv_obj_set_pos(sub, 20, 44);

    /* ---- 亮度 ---- */
    lv_obj_t *bl = ui_label(s_cc, "亮度", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(bl, 20, 82);

    s_cc_brightness = lv_slider_create(s_cc);
    lv_obj_set_pos(s_cc_brightness, 74, 86);
    lv_obj_set_size(s_cc_brightness, 306, 10);
    lv_slider_set_range(s_cc_brightness, 5, 100);
    lv_slider_set_value(s_cc_brightness, s_brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_cc_brightness, col(0x2A2F45), LV_PART_MAIN);
    lv_obj_set_style_radius(s_cc_brightness, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cc_brightness, col(0x8B5CF6), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_cc_brightness, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_cc_brightness, col(0xC7B4FF), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_cc_brightness, 0, LV_PART_KNOB);
    lv_obj_set_style_width(s_cc_brightness, 6, LV_PART_KNOB);
    lv_obj_set_style_height(s_cc_brightness, 18, LV_PART_KNOB);
    lv_obj_add_event_cb(s_cc_brightness, cc_brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_cc_bright_val = ui_label(s_cc, "", &lv_font_montserrat_20, 0x8B5CF6);
    lv_label_set_text_fmt(s_cc_bright_val, "%d", s_brightness);
    lv_obj_set_style_text_align(s_cc_bright_val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(s_cc_bright_val, 60);
    lv_obj_set_pos(s_cc_bright_val, 398, 80);

    /* ---- WiFi 卡片 ----
     * 这里不再放开关（射频开关只在下面「WiFi 射频」那张卡上），
     * 右侧留一个「扫描 WiFi」按钮，点它进附近网络列表。 */
    lv_obj_t *wc = cc_card(s_cc, 112, 132);
    lv_obj_t *wt = ui_label(wc, "WiFi", &ui_cjk_24, C_TXT);
    lv_obj_set_pos(wt, 16, 12);

    s_cc_wifi_state = ui_label(wc, "未连接", &ui_cjk_16, 0x22D3EE);
    lv_obj_set_pos(s_cc_wifi_state, 16, 52);
    lv_obj_set_width(s_cc_wifi_state, 262);
    lv_label_set_long_mode(s_cc_wifi_state, LV_LABEL_LONG_DOT);

    s_cc_wifi_ip = ui_label(wc, "--", &ui_cjk_16, 0x8A93A6);
    lv_obj_set_pos(s_cc_wifi_ip, 16, 80);
    lv_obj_set_width(s_cc_wifi_ip, 262);
    lv_label_set_long_mode(s_cc_wifi_ip, LV_LABEL_LONG_DOT);
    /* 点这两行也能进列表（已保存的网络在那里可以「忘记」） */
    lv_obj_add_flag(s_cc_wifi_state, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cc_wifi_state, scan_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_cc_wifi_ip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cc_wifi_ip, scan_open_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *scan = lv_button_create(wc);
    lv_obj_set_size(scan, 124, 42);
    lv_obj_set_pos(scan, 300, 45);
    lv_obj_set_style_radius(scan, 12, 0);
    lv_obj_set_style_bg_color(scan, col(0x1B2334), 0);
    lv_obj_set_style_border_width(scan, 1, 0);
    lv_obj_set_style_border_color(scan, col(0x3D4B6D), 0);
    lv_obj_set_style_pad_all(scan, 0, 0);
    lv_obj_add_event_cb(scan, scan_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = ui_label(scan, "扫描 WiFi", &ui_cjk_16, 0x9D7CFF);
    lv_obj_center(sl);

    /* ---- 蓝牙卡片 ---- */
    lv_obj_t *bc = cc_card(s_cc, 256, 72);
    lv_obj_t *bt = ui_label(bc, "蓝牙 ESP32-P4C5", &ui_cjk_24, C_TXT);
    lv_obj_set_pos(bt, 16, 18);
    s_cc_bt_sw = cc_switch(bc, 368, 21, bt_mgr_is_on(), bt_switch_cb);

    /* ---- WiFi 射频卡片 ---- */
    lv_obj_t *rc = cc_card(s_cc, 340, 72);
    lv_obj_t *rt = ui_label(rc, "WiFi 射频", &ui_cjk_24, C_TXT);
    lv_obj_set_pos(rt, 16, 18);
    lv_obj_t *rh = ui_label(rc, "关闭可省电", &ui_cjk_16, 0x8A93A6);
    lv_obj_align(rh, LV_ALIGN_TOP_RIGHT, -86, 26);
    s_cc_rf_sw = cc_switch(rc, 368, 21, wifi_mgr_is_on(), wifi_switch_cb);

    /* ---- 底部收起 ---- */
    lv_obj_t *close = lv_button_create(s_cc);
    lv_obj_set_size(close, 360, 42);
    lv_obj_set_pos(close, 60, 428);
    lv_obj_set_style_radius(close, 21, 0);
    lv_obj_set_style_bg_color(close, col(0x8B5CF6), 0);
    lv_obj_add_event_cb(close, cc_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = ui_label(close, "点击收起", &ui_cjk_16, C_TXT);
    lv_obj_center(cl);

    build_scan_modal();

    /* ---- 密码弹窗（独立，盖在控制中心上） ---- */
    s_pw_modal = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_pw_modal, 480, 480);
    lv_obj_set_pos(s_pw_modal, 0, 0);
    lv_obj_set_style_bg_color(s_pw_modal, col(0x000000), 0);
    lv_obj_set_style_bg_opa(s_pw_modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_pw_modal, 0, 0);
    lv_obj_set_style_radius(s_pw_modal, 0, 0);
    lv_obj_set_style_pad_all(s_pw_modal, 0, 0);
    lv_obj_add_flag(s_pw_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(s_pw_modal);
    lv_obj_set_size(card, 460, 430);
    lv_obj_set_pos(card, 10, 20);
    lv_obj_set_style_bg_color(card, col(0x10254A), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = ui_label(card, "输入密码", NULL, C_TXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 2);
    lv_obj_set_user_data(s_pw_modal, title);

    lv_obj_t *ta = lv_textarea_create(card);
    lv_obj_set_size(ta, 440, 48);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 28);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_placeholder_text(ta, "Wi-Fi 密码");
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    s_pw_ta = ta;

    lv_obj_t *kb = lv_keyboard_create(card);
    lv_obj_set_size(kb, 440, 190);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 84);
    lv_keyboard_set_textarea(kb, ta);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 130, 40);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 30, 282);
    lv_obj_set_style_bg_color(cancel, col(0x39496B), 0);
    lv_obj_add_event_cb(cancel, pw_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl2 = ui_label(cancel, "取消", NULL, C_TXT);
    lv_obj_center(cl2);

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_set_size(ok, 130, 40);
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -30, 282);
    lv_obj_set_style_bg_color(ok, col(C_ACCENT), 0);
    lv_obj_add_event_cb(ok, pw_connect_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = ui_label(ok, "连接", NULL, C_TXT);
    lv_obj_center(ol);
}

static void cc_open(void)
{
    lv_obj_set_y(s_cc, -SHELL_PREVIEW_H);
    lv_obj_remove_flag(s_cc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_cc);
    if (s_scan_modal) {
        lv_obj_move_foreground(s_scan_modal);
    }
    lv_obj_move_foreground(s_pw_modal);
    slide_y(s_cc, -SHELL_PREVIEW_H, 0, 260, false);
    cc_refresh();
    /* 不再每次进来都强制扫描：开机扫过一次、掉线补扫一次就够，
     * 面板上的「扫描」按钮仍可手动强扫（省电、不抢射频） */
}
static void cc_refresh(void)
{
    wifi_mgr_state_t st = wifi_mgr_get_state();
    bool radio = wifi_mgr_is_on();
    const char *ssid = wifi_mgr_get_ssid();

    if (!radio) {
        lv_label_set_text(s_cc_wifi_state, "WiFi 已关闭");
        lv_obj_set_style_text_color(s_cc_wifi_state, col(0x8A93A6), 0);
    } else if (st == WIFI_MGR_CONNECTED) {
        char b[64];
        snprintf(b, sizeof(b), "%s 已连接", ssid[0] ? ssid : "-");
        lv_label_set_text(s_cc_wifi_state, ui_font_safe(b, &ui_cjk_16));
        lv_obj_set_style_text_color(s_cc_wifi_state, col(0x22D3EE), 0);
    } else if (wifi_mgr_auth_failed() && st != WIFI_MGR_CONNECTED) {
        lv_label_set_text(s_cc_wifi_state, "密码错误，请重新输入");
        lv_obj_set_style_text_color(s_cc_wifi_state, col(0xFF6B6B), 0);
    } else if (st == WIFI_MGR_CONNECTING) {
        char b[64];
        snprintf(b, sizeof(b), "正在连接 %s…", ssid[0] ? ssid : "");
        lv_label_set_text(s_cc_wifi_state, ui_font_safe(b, &ui_cjk_16));
        lv_obj_set_style_text_color(s_cc_wifi_state, col(0xE8C537), 0);
    } else if (st == WIFI_MGR_SCANNING) {
        lv_label_set_text(s_cc_wifi_state, "正在扫描附近 WiFi…");
        lv_obj_set_style_text_color(s_cc_wifi_state, col(0xE8C537), 0);
    } else {
        lv_label_set_text(s_cc_wifi_state, "未连接");
        lv_obj_set_style_text_color(s_cc_wifi_state, col(0x22D3EE), 0);
    }

    /* 第二行：另一张已保存的网络；没有就显示 IP */
    const char *other = NULL;
    for (int i = 0; i < wifi_mgr_saved_count(); i++) {
        const char *s2 = wifi_mgr_saved_ssid_at(i);
        if (s2[0] && strcmp(s2, ssid) != 0) {
            other = s2;
            break;
        }
    }
    if (other) {
        lv_label_set_text(s_cc_wifi_ip, ui_font_safe(other, &ui_cjk_16));
        lv_obj_set_style_text_color(s_cc_wifi_ip, col(0x8A93A6), 0);
    } else if (radio && st == WIFI_MGR_CONNECTED) {
        lv_label_set_text_fmt(s_cc_wifi_ip, "IP %s", wifi_mgr_get_ip());
        lv_obj_set_style_text_color(s_cc_wifi_ip, col(0x8A93A6), 0);
    } else {
        lv_label_set_text(s_cc_wifi_ip,
                          radio ? "点右侧「扫描 WiFi」选网络" : "打开 WiFi 射频后自动连接");
        lv_obj_set_style_text_color(s_cc_wifi_ip, col(0x6B7488), 0);
    }

    /* 射频开关跟随实际状态（抑制回调，避免递归） */
    if (s_cc_rf_sw) {
        bool cur = lv_obj_has_state(s_cc_rf_sw, LV_STATE_CHECKED);
        if (cur != radio) {
            s_sw_sync = true;
            if (radio) {
                lv_obj_add_state(s_cc_rf_sw, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_cc_rf_sw, LV_STATE_CHECKED);
            }
            s_sw_sync = false;
        }
    }
    if (s_cc_bt_sw) {
        bool cur = lv_obj_has_state(s_cc_bt_sw, LV_STATE_CHECKED);
        bool on = bt_mgr_is_on();
        if (cur != on) {
            s_sw_sync = true;
            if (on) {
                lv_obj_add_state(s_cc_bt_sw, LV_STATE_CHECKED);
            } else {
                lv_obj_clear_state(s_cc_bt_sw, LV_STATE_CHECKED);
            }
            s_sw_sync = false;
        }
    }

    if (s_ap_list_dirty) {
        s_ap_list_dirty = false;
        ap_list_rebuild();
    }
}

static void build_lock_screen(void)
{
    s_lockscr = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_lockscr, 480, 480);
    lv_obj_set_pos(s_lockscr, 0, 0);
    /* 深蓝夜空渐变，比纯黑好看 */
    lv_obj_set_style_bg_color(s_lockscr, col(0x122541), 0);
    lv_obj_set_style_bg_grad_color(s_lockscr, col(0x04070F), 0);
    lv_obj_set_style_bg_grad_dir(s_lockscr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(s_lockscr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lockscr, 0, 0);
    lv_obj_set_style_radius(s_lockscr, 0, 0);
    lv_obj_set_style_pad_all(s_lockscr, 0, 0);
    lv_obj_clear_flag(s_lockscr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_lockscr, lock_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_lockscr, global_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(s_lockscr, LV_OBJ_FLAG_HIDDEN);

    /* 日期（时间上方，居中） */
    s_lock_date = ui_label(s_lockscr, "--/--  ----", &lv_font_montserrat_20, 0x8FA8CC);
    lv_obj_align(s_lock_date, LV_ALIGN_TOP_MID, 0, 104);

    /* 超大时间（96px 数字字体，居中） */
    s_lock_time = lv_label_create(s_lockscr);
    lv_label_set_text(s_lock_time, "--:--");
    lv_obj_set_style_text_font(s_lock_time, &ui_font_time96, 0);
    lv_obj_set_style_text_color(s_lock_time, col(0xFFFFFF), 0);
    lv_obj_align(s_lock_time, LV_ALIGN_TOP_MID, 0, 138);

    /* 分隔线 */
    lv_obj_t *line = lv_obj_create(s_lockscr);
    lv_obj_set_size(line, 120, 2);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 268);
    lv_obj_set_style_bg_color(line, col(0x3A5478), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 1, 0);

    /* 天气描述 + 温度 */
    s_lock_wx = ui_label(s_lockscr, "", &ui_cjk_16, 0xDCE6F2);
    lv_obj_align(s_lock_wx, LV_ALIGN_TOP_MID, 0, 288);

    /* 城市 + 今日温度范围 */
    s_lock_city = ui_label(s_lockscr, "", NULL, 0x8FA8CC);
    lv_obj_align(s_lock_city, LV_ALIGN_TOP_MID, 0, 328);

    /* 星期（中文字，必须用中文字体） */
    s_lock_week = ui_label(s_lockscr, "---", &ui_cjk_16, 0xB8C4D8);
    lv_obj_align(s_lock_week, LV_ALIGN_TOP_MID, 0, 366);

    /* 底部提示 */
    lv_obj_t *hint = ui_label(s_lockscr, "轻触任意位置解锁", NULL, 0x4E6080);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -30);
}
static void lock_update(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2024) {
        lv_label_set_text_fmt(s_lock_time, "%02d:%02d", t.tm_hour, t.tm_min);
        lv_label_set_text_fmt(s_lock_date, "%02d/%02d  %d",
                              t.tm_mon + 1, t.tm_mday, t.tm_year + 1900);
        static const char *wd[] = { "星期日", "星期一", "星期二", "星期三",
                                    "星期四", "星期五", "星期六" };
        lv_label_set_text(s_lock_week, wd[t.tm_wday]);
    } else {
        lv_label_set_text(s_lock_time, "--:--");
        lv_label_set_text(s_lock_date, "--/--  ----");
        lv_label_set_text(s_lock_week, "---");
    }

    float temp = 0, tmax = 0, tmin = 0;
    int code = 0;
    const char *city = NULL;
    if (ui_weather_summary(&temp, &tmax, &tmin, &code, &city)) {
        const char *desc = ui_weather_desc();
        lv_label_set_text_fmt(s_lock_wx, "%s  %.0f度", desc, temp);
        lv_label_set_text_fmt(s_lock_city, "%s   %.0f~%.0f度", city ? city : "", tmin, tmax);
    } else {
        lv_label_set_text(s_lock_wx, "");
        lv_label_set_text(s_lock_city, "天气加载中");
    }
}
static void lock_click_cb(lv_event_t *e)
{
    (void)e;
    do_unlock();
}
void ui_shell_set_rotation(ui_rot_t rot)
{
    if (rot > UI_ROT_270) {
        rot = UI_ROT_0;
    }
    s_rotation = rot;
    lv_display_rotation_t lr = LV_DISPLAY_ROTATION_0;
    switch (rot) {
    case UI_ROT_90:  lr = LV_DISPLAY_ROTATION_90;  break;
    case UI_ROT_180: lr = LV_DISPLAY_ROTATION_180; break;
    case UI_ROT_270: lr = LV_DISPLAY_ROTATION_270; break;
    default:         lr = LV_DISPLAY_ROTATION_0;   break;
    }
    if (s_disp) {
        lv_display_set_rotation(s_disp, lr);
    }
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "disp_rot", (int32_t)rot);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "display rotation -> %d", (int)rot);
}
void ui_shell_set_tick_cb(void (*tick)(void))
{
    s_tick_cb = tick;
}

void ui_shell_wifi_notify(void *arg)
{
    (void)arg;
    s_wifi_dirty = true;
    s_ap_list_dirty = true;
}

/* 开机进锁屏 / 手动锁屏：force=true 时跳过开机 3 秒与 900ms 防抖 */
static void do_lock_ex(bool force)
{
    if (s_locked || s_lockscr == NULL) {
        return;
    }
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (!force && now_ms - s_boot_ms < 3000) {
        ESP_LOGW(TAG, "ignore lock during boot");
        return;
    }
    if (!force && now_ms - s_lock_ts < 900) {
        return;
    }
    s_lock_ts = now_ms;
    s_locked = true;
    /* 收起控制中心，避免锁屏下误触 */
    if (s_cc) {
        lv_obj_add_flag(s_cc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_y(s_cc, -SHELL_PREVIEW_H);
    }
    lock_update();
    lv_obj_remove_flag(s_lockscr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lockscr);
    /* 锁屏过渡：从下方滑入 + 淡入 */
    lv_obj_set_y(s_lockscr, 70);
    lv_obj_set_style_opa(s_lockscr, 0, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lockscr);
    lv_anim_set_values(&a, 70, 0);
    lv_anim_set_time(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, anim_y_exec);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, s_lockscr);
    lv_anim_set_values(&b, 0, 255);
    lv_anim_set_time(&b, 220);
    lv_anim_set_exec_cb(&b, anim_opa_exec);
    lv_anim_start(&b);

    backlight_duty((uint32_t)LOCK_BRIGHTNESS * 1023 / 100);
    s_last_active_ms = now_ms;
    ESP_LOGI(TAG, "locked (screen dimmed to %d%%)", LOCK_BRIGHTNESS);
}

static void do_lock(void)
{
    do_lock_ex(false);
}

/* 锁屏信息（时间/日期/天气）是否都出来了：开机自检要等这个 */
static void shell_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* 1. 相机帧刷新 */
    if (s_frame_latest >= 0 && s_frame_latest != s_frame_shown) {
        s_frame_shown = s_frame_latest;
        /* 注意：这里必须扫全部槽位。之前用 s_listener_cnt 做上界，
         * 而 attach 走的是"复用空槽"分支、cnt 一直是 0，
         * 循环一次都不执行 —— 表现就是取景画面永远停在进 App 的那一帧。 */
        for (int i = 0; i < SHELL_PREVIEW_LISTENERS; i++) {
            if (s_listeners[i].img) {
                lv_image_set_src(s_listeners[i].img, &s_dsc[s_frame_shown]);
                lv_obj_invalidate(s_listeners[i].img);
            }
        }
    }

    /* 2. WiFi 事件 */
    if (s_wifi_dirty) {
        s_wifi_dirty = false;
        statusbar_wifi_update();
        if (!lv_obj_has_flag(s_cc, LV_OBJ_FLAG_HIDDEN)) {
            cc_refresh();
        }
    }

    /* 2.5 全局提醒（价格提醒等） */
    if (s_alert_pending) {
        s_alert_pending = false;
        alert_show_card();
    }
    if (s_alert_card && esp_timer_get_time() / 1000 > s_alert_until) {
        lv_obj_delete(s_alert_card);
        s_alert_card = NULL;
    }

    /* 3. 时钟（500ms 节流） */
    static int div;
    if (++div >= 16) {
        div = 0;
        statusbar_update();
        if (s_locked && !s_sleeping) {
            lock_update();
        }
        /* App 内可能新建了控件，定期补上事件冒泡（手势依赖） */
        if (s_app_active >= 0) {
            enable_bubble_recursive(s_app_container[s_app_active]);
        }
        if (s_tick_cb) {
            s_tick_cb();
        }
    }

    /* 4. 待机检测（每 30ms） */
    idle_check();
}

bool ui_shell_lock_ready(void)
{
    if (!s_lock_time) {
        return true;
    }
    const char *t = lv_label_get_text(s_lock_time);
    if (!t || !t[0] || strcmp(t, "--:--") == 0) {
        return false;
    }
    if (s_lock_date) {
        const char *d = lv_label_get_text(s_lock_date);
        if (!d || strstr(d, "--/--")) {
            return false;
        }
    }
    /* 天气没到之前不收起自检（天气 App 关掉时不要求） */
    if (s_lock_city && ui_shell_app_on_by_name("天气")) {
        const char *c = lv_label_get_text(s_lock_city);
        if (!c || !c[0] || strstr(c, "加载中")) {
            return false;
        }
    }
    return true;
}


const lv_image_dsc_t *ui_shell_preview_dsc(int idx)
{
    if (idx < 0 || idx >= SHELL_PREVIEW_NUM) {
        return NULL;
    }
    return &s_dsc[idx];
}

const lv_image_dsc_t *ui_shell_preview_latest_dsc(void)
{
    if (s_frame_latest < 0) {
        return NULL;
    }
    return &s_dsc[s_frame_latest];
}

/* 预览图被删除时（App 退出销毁界面）要摘掉监听，
 * 否则定时器会往已释放的对象上 set_src → 崩 */
static void preview_img_deleted_cb(lv_event_t *e)
{
    lv_obj_t *img = lv_event_get_target(e);
    for (int i = 0; i < SHELL_PREVIEW_LISTENERS; i++) {
        if (s_listeners[i].img == img) {
            s_listeners[i].img = NULL;
        }
    }
}

lv_obj_t *ui_shell_preview_attach(lv_obj_t *parent)
{
    lv_obj_t *img = lv_image_create(parent);
    if (s_frame_shown >= 0) {
        lv_image_set_src(img, &s_dsc[s_frame_shown]);
    } else {
        lv_obj_set_style_bg_color(img, col(0x05080F), 0);
        lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
    }
    /* 复用空槽位：反复进出相机/监控也能一直拿到监听位 */
    int slot = -1;
    for (int i = 0; i < SHELL_PREVIEW_LISTENERS; i++) {
        if (s_listeners[i].img == NULL) {
            slot = i;
            break;
        }
    }
    if (slot >= 0) {
        s_listeners[slot].img = img;
        if (slot >= s_listener_cnt) {
            s_listener_cnt = slot + 1;      /* 上界只增不减，槽位回收也不缩 */
        }
        lv_obj_add_event_cb(img, preview_img_deleted_cb, LV_EVENT_DELETE, NULL);
    }
    return img;
}

ui_rot_t ui_shell_get_rotation(void)
{
    return s_rotation;
}

/* 相机画面单独转：屏幕换成纵向/反向时，取景画面也要跟着转，
 * 否则 UI 转正了、画面还是横的。角度按逆时针（与 LVGL/PPA 一致）。 */
int ui_shell_camera_rotation(void)
{
    switch (s_rotation) {
    case UI_ROT_90:  return 90;
    case UI_ROT_180: return 180;
    case UI_ROT_270: return 270;
    default:         return 0;
    }
}

void ui_shell_set_gesture_enabled(bool on)
{
    s_gesture_enabled = on;
}

void ui_shell_preview_clear(void)
{
    for (int i = 0; i < SHELL_PREVIEW_NUM; i++) {
        if (s_pbuf[i]) {
            memset(s_pbuf[i], 0, SHELL_PREVIEW_W * SHELL_PREVIEW_H * 2);
        }
    }
}

void ui_shell_register_app(const ui_app_t *app)
{
    if (s_app_cnt < MAX_APPS) {
        s_apps[s_app_cnt++] = app;
    }
}

/* ================= App 开关（NVS 位图） ================= */

#define NVS_NS_SYS  "sys"
#define NVS_KEY_APP "app_off"
#define NVS_KEY_BG  "app_bg"

static void app_off_load(void)
{
    s_app_off = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS_SYS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, NVS_KEY_APP, &v) == ESP_OK) {
            s_app_off = (uint32_t)v;
        }
        nvs_close(h);
    }
    s_app_bg = 0;
    if (nvs_open(NVS_NS_SYS, NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, NVS_KEY_BG, &v) == ESP_OK) {
            s_app_bg = (uint32_t)v;
        }
        nvs_close(h);
    }
    /* R-33：地图 App 冻结后注册顺序变短了一位（旧：…3交易 4导航 5音乐 6天气 7设置），
     * 开关是按索引位图存的，不迁移会出现"关了导航，升级后关掉的是音乐"。
     * 这里按老顺序重映射一次：bit4(导航) 丢弃，5→4、6→5、7→6。 */
    {
        nvs_handle_t hv;
        uint8_t ver = 0;
        bool need = false;
        if (nvs_open(NVS_NS_SYS, NVS_READWRITE, &hv) == ESP_OK) {
            if (nvs_get_u8(hv, "app_off_ver", &ver) != ESP_OK || ver < 2) {
                need = true;
            }
            if (need) {
                uint32_t old_off = s_app_off, old_bg = s_app_bg;
                s_app_off = (old_off & 0x0F) | ((old_off >> 1) & 0x70);
                s_app_bg  = (old_bg  & 0x0F) | ((old_bg  >> 1) & 0x70);
                nvs_set_u8(hv, "app_off_ver", 2);
                nvs_set_i32(hv, NVS_KEY_APP, (int32_t)s_app_off);
                nvs_set_i32(hv, NVS_KEY_BG, (int32_t)s_app_bg);
                nvs_commit(hv);
                ESP_LOGW(TAG, "app mask migrated(v2): off 0x%02lx->0x%02lx bg 0x%02lx->0x%02lx",
                         (unsigned long)old_off, (unsigned long)s_app_off,
                         (unsigned long)old_bg, (unsigned long)s_app_bg);
            }
            nvs_close(hv);
        }
    }

    ESP_LOGI(TAG, "app switch mask = 0x%02lx, bg mask = 0x%02lx",
             (unsigned long)s_app_off, (unsigned long)s_app_bg);
    s_app_shutting = false;
}

static void app_off_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS_SYS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_APP, (int32_t)s_app_off);
        nvs_set_i32(h, NVS_KEY_BG, (int32_t)s_app_bg);
        nvs_commit(h);
        nvs_close(h);
    }
}

int ui_shell_app_count(void)
{
    return s_app_cnt;
}

const char *ui_shell_app_name(int idx)
{
    return (idx >= 0 && idx < s_app_cnt) ? s_apps[idx]->name : "";
}

bool ui_shell_app_enabled(int idx)
{
    if (idx < 0 || idx >= s_app_cnt) {
        return false;
    }
    if (s_apps[idx]->always_on) {
        return true;
    }
    return (s_app_off & (1u << idx)) == 0;
}

bool ui_shell_app_on_by_name(const char *name)
{
    if (!name) {
        return false;
    }
    for (int i = 0; i < s_app_cnt; i++) {
        if (strcmp(s_apps[i]->name, name) == 0) {
            return ui_shell_app_enabled(i);
        }
    }
    return false;
}

/* ================= 桌面图标（可控重建） ================= */

static void desktop_pages_build(void)
{
    static const int cx[3] = { 90, 240, 390 };
    static const int cy[2] = { 56, 214 };

    int on_cnt = 0;
    for (int i = 0; i < s_app_cnt; i++) {
        if (ui_shell_app_enabled(i)) {
            on_cnt++;
        }
    }
    int page_cnt = (on_cnt + 5) / 6;
    if (page_cnt < 1) {
        page_cnt = 1;
    }

    s_pages = lv_obj_create(s_desktop);
    lv_obj_set_size(s_pages, 480, 442);
    lv_obj_set_pos(s_pages, 0, 30);
    lv_obj_set_style_bg_opa(s_pages, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pages, 0, 0);
    lv_obj_set_style_pad_all(s_pages, 0, 0);
    lv_obj_set_style_radius(s_pages, 0, 0);
    lv_obj_set_scroll_dir(s_pages, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_pages, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_snap_x(s_pages, LV_SCROLL_SNAP_CENTER);
    lv_obj_add_event_cb(s_pages, global_gesture_cb, LV_EVENT_GESTURE, NULL);

    for (int p = 0; p < page_cnt; p++) {
        lv_obj_t *pg = lv_obj_create(s_pages);
        lv_obj_set_size(pg, 480, 442);
        lv_obj_set_pos(pg, p * 480, 0);
        lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(pg, 0, 0);
        lv_obj_set_style_pad_all(pg, 0, 0);
        lv_obj_set_style_radius(pg, 0, 0);
        lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    }

    int cell = 0;
    for (int i = 0; i < s_app_cnt; i++) {
        if (!ui_shell_app_enabled(i)) {
            continue;
        }
        int page = cell / 6, idx_in_page = cell % 6;
        int row = idx_in_page / 3, colI = idx_in_page % 3;
        int x = cx[colI] - 50;
        int y = cy[row];
        cell++;
        lv_obj_t *pg = lv_obj_get_child(s_pages, page);

        lv_obj_t *icon = lv_obj_create(pg);
        lv_obj_set_size(icon, 100, 100);
        lv_obj_set_pos(icon, x, y);
        lv_obj_set_style_radius(icon, 24, 0);
        lv_obj_set_style_bg_color(icon, col(s_apps[i]->color), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_shadow_width(icon, 30, 0);
        lv_obj_set_style_shadow_color(icon, col(0x000000), 0);
        lv_obj_set_style_shadow_opa(icon, LV_OPA_40, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(icon, desktop_app_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *sym = lv_label_create(icon);
        lv_label_set_text(sym, s_apps[i]->symbol);
        lv_obj_set_style_text_font(sym, &lv_font_montserrat_32, 0);
        lv_obj_set_style_text_color(sym, col(0xFFFFFF), 0);
        lv_obj_center(sym);

        lv_obj_t *name = ui_label(pg, s_apps[i]->name, NULL, C_TXT);
        lv_obj_set_pos(name, x - 10, y + 104);
        lv_obj_set_width(name, 120);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    }

    /* 桌面图标也要能把手势上交给分页容器 */
    enable_bubble_recursive(s_pages);

    /* 分页容器插在状态栏下面、锁屏横条上面（否则会盖住底部横条） */
    lv_obj_move_to_index(s_pages, 1);
}

/* 改完开关重建桌面（只重建图标层，App 容器不受影响） */
static void desktop_rebuild(void)
{
    if (!s_desktop) {
        return;
    }
    if (s_pages) {
        lv_obj_delete(s_pages);
        s_pages = NULL;
    }
    desktop_pages_build();
}

bool ui_shell_app_bg(int idx)
{
    if (idx < 0 || idx >= s_app_cnt) {
        return false;
    }
    return (s_app_bg & (1u << idx)) != 0;
}

void ui_shell_app_bg_set(int idx, bool on)
{
    if (idx < 0 || idx >= s_app_cnt) {
        return;
    }
    if (on) {
        s_app_bg |= (1u << idx);
    } else {
        s_app_bg &= ~(1u << idx);
    }
    app_off_save();
    ESP_LOGI(TAG, "app[%d] \"%s\" background -> %s", idx, s_apps[idx]->name, on ? "ON" : "OFF");
}

bool ui_shell_app_bg_by_name(const char *name)
{
    if (!name) {
        return false;
    }
    for (int i = 0; i < s_app_cnt; i++) {
        if (strcmp(s_apps[i]->name, name) == 0) {
            return (s_app_bg & (1u << i)) != 0;
        }
    }
    return false;
}

bool ui_shell_app_shutting(void)
{
    return s_app_shutting;
}

void ui_shell_set_app_enabled(int idx, bool on)
{
    if (idx < 0 || idx >= s_app_cnt) {
        return;
    }
    if (!on && s_apps[idx]->always_on) {
        ui_shell_toast("该 App 不能关闭");
        return;
    }
    if (ui_shell_app_enabled(idx) == on) {
        return;
    }
    if (on) {
        s_app_off &= ~(1u << idx);
    } else {
        s_app_off |= (1u << idx);
    }
    app_off_save();

    /* 正在用这个 App 就退回桌面，别让关掉的界面留在屏幕上；
     * 关掉的 App 连同已创建的界面一起释放 */
    if (!on && s_app_active == idx) {
        ui_shell_back_event_cb(NULL);
    }
    if (!on && s_app_container[idx]) {
        lv_obj_delete(s_app_container[idx]);
        s_app_container[idx] = NULL;
        s_app_created[idx] = false;
    }
    desktop_rebuild();
    ESP_LOGI(TAG, "app[%d] \"%s\" -> %s", idx, s_apps[idx]->name, on ? "ON" : "OFF");
}

esp_err_t ui_shell_start(esp_lcd_panel_handle_t panel_handle, esp_lcd_touch_handle_t touch)
{
    brightness_init();
    app_off_load();
      /* 先读 App 开关，桌面只建启用的 */

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 4096 * 3,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 5,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "lvgl_port_init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = app_lcd_get_io(),
        .panel_handle = panel_handle,
        .buffer_size = SHELL_PREVIEW_W * SHELL_PREVIEW_H,
        .double_buffer = true,
        .hres = SHELL_PREVIEW_W,
        .vres = SHELL_PREVIEW_H,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_dsi_cfg_t dpi_cfg = { .flags = { .avoid_tearing = false } };
    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "add disp failed");
    s_disp = disp;

    /* 恢复上次选择的显示方向（默认横向） */
    {
        nvs_handle_t rh;
        int32_t rot = 0;
        if (nvs_open("sys", NVS_READONLY, &rh) == ESP_OK) {
            if (nvs_get_i32(rh, "disp_rot", &rot) == ESP_OK && rot >= 0 && rot <= 3) {
                s_rotation = (ui_rot_t)rot;
            }
            nvs_close(rh);
        }
        if (s_rotation != UI_ROT_0) {
            lv_display_rotation_t lr = (s_rotation == UI_ROT_90)  ? LV_DISPLAY_ROTATION_90 :
                                       (s_rotation == UI_ROT_180) ? LV_DISPLAY_ROTATION_180 :
                                                                    LV_DISPLAY_ROTATION_270;
            lv_display_set_rotation(disp, lr);
            ESP_LOGI(TAG, "display rotation restored: %d", (int)s_rotation);
        }
    }

    if (touch) {
        /* 丢弃上电瞬间的触摸脏数据：ST7123 首次采样偶尔会报"按下+错误坐标"，
         * 会被 LVGL 当成点击，导致开机自己打开 App 或触发锁屏 */
        for (int i = 0; i < 4; i++) {
            esp_lcd_touch_read_data(touch);
            vTaskDelay(pdMS_TO_TICKS(25));
        }
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = touch,
        };
        lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
        if (indev == NULL) {
            ESP_LOGW(TAG, "add touch indev failed, UI runs without touch");
        } else {
            /* 加快触摸读取周期，提升灵敏度（默认 LV_DEF_REFR_PERIOD，改为 10ms） */
            lv_timer_t *read_timer = lv_indev_get_read_timer(indev);
            if (read_timer) {
                lv_timer_set_period(read_timer, 10);
            }
            /* 全局按压反馈动画：任意可点控件按下缩小 92%，松开弹回 */
            lv_indev_add_event_cb(indev, press_feedback, LV_EVENT_PRESSED, NULL);
            lv_indev_add_event_cb(indev, press_feedback, LV_EVENT_RELEASED, NULL);
            lv_indev_add_event_cb(indev, press_feedback, LV_EVENT_PRESS_LOST, NULL);
        }
    }

    /* 预览缓冲（PSRAM，64 字节对齐满足 PPA/DMA） */
    for (int i = 0; i < SHELL_PREVIEW_NUM; i++) {
        s_pbuf[i] = heap_caps_aligned_alloc(64, SHELL_PREVIEW_W * SHELL_PREVIEW_H * 2,
                                            MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_pbuf[i], ESP_ERR_NO_MEM, TAG, "preview buf alloc failed");
        /* 预清零：非 1:1 画幅时上下留黑边，黑边就是这里留下的 */
        memset(s_pbuf[i], 0, SHELL_PREVIEW_W * SHELL_PREVIEW_H * 2);
        memset(&s_dsc[i], 0, sizeof(lv_image_dsc_t));
        s_dsc[i].header.cf = LV_COLOR_FORMAT_RGB565;
        s_dsc[i].header.w = SHELL_PREVIEW_W;
        s_dsc[i].header.h = SHELL_PREVIEW_H;
        s_dsc[i].header.stride = SHELL_PREVIEW_W * 2;
        s_dsc[i].data = s_pbuf[i];
        s_dsc[i].data_size = SHELL_PREVIEW_W * SHELL_PREVIEW_H * 2;
    }

    lvgl_port_lock(0);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, col(C_BG), 0);
    lv_obj_add_event_cb(scr, global_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* ---- 桌面 ---- */
    s_desktop = lv_obj_create(scr);
    lv_obj_set_size(s_desktop, 480, 480);
    lv_obj_set_pos(s_desktop, 0, 0);
    lv_obj_set_style_bg_color(s_desktop, col(C_BG), 0);
    lv_obj_set_style_bg_opa(s_desktop, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_desktop, 0, 0);
    lv_obj_set_style_pad_all(s_desktop, 0, 0);
    lv_obj_set_style_radius(s_desktop, 0, 0);
    lv_obj_clear_flag(s_desktop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_desktop, global_gesture_cb, LV_EVENT_GESTURE, NULL);

    /* 状态栏 */
    lv_obj_t *bar = lv_obj_create(s_desktop);
    lv_obj_set_size(bar, 480, 30);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, col(0x081226), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bar, statusbar_click_cb, LV_EVENT_CLICKED, NULL);

    s_statusbar_time = lv_label_create(bar);
    lv_label_set_text(s_statusbar_time, "--:--");
    lv_obj_set_style_text_font(s_statusbar_time, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_statusbar_time, col(C_TXT), 0);
    lv_obj_align(s_statusbar_time, LV_ALIGN_LEFT_MID, 10, 0);

    s_statusbar_wifi = lv_label_create(bar);
    lv_label_set_text(s_statusbar_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(s_statusbar_wifi, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_statusbar_wifi, col(0x5A6B8C), 0);
    lv_obj_align(s_statusbar_wifi, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 居中显示当前 SSID（点击整条状态栏打开控制中心） */
    s_statusbar_ssid = lv_label_create(bar);
    lv_label_set_text(s_statusbar_ssid, "未连接");
    lv_obj_set_style_text_font(s_statusbar_ssid, &ui_cjk_16, 0);
    lv_obj_set_style_text_color(s_statusbar_ssid, col(0x5A6B8C), 0);
    lv_obj_align(s_statusbar_ssid, LV_ALIGN_CENTER, 0, 0);

    /* App 图标：一页 6 个（3x2），左右滑动翻页；只放已启用的 App */
    desktop_pages_build();

    build_control_center();
    build_lock_screen();

    /* 桌面底部「上滑锁屏待机」横条 */
    lv_obj_t *lockbar = lv_obj_create(s_desktop);
    lv_obj_set_size(lockbar, 480, 46);
    lv_obj_set_pos(lockbar, 0, 480 - 46);
    lv_obj_set_style_bg_color(lockbar, col(0x0D1D3D), 0);
    lv_obj_set_style_bg_opa(lockbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(lockbar, 0, 0);
    lv_obj_set_style_radius(lockbar, 0, 0);
    lv_obj_set_style_pad_all(lockbar, 0, 0);
    lv_obj_clear_flag(lockbar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(lockbar, global_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_t *lb = ui_label(lockbar, "^  上滑锁屏待机", NULL, 0x5C7BA8);
    lv_obj_center(lb);

    /* 待机息屏遮罩：黑色全屏，点击即唤醒（吞掉这一次点击，不触发下层按钮） */
    s_sleep_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_sleep_overlay, 480, 480);
    lv_obj_set_pos(s_sleep_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_sleep_overlay, col(0x000000), 0);
    lv_obj_set_style_bg_opa(s_sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_sleep_overlay, 0, 0);
    lv_obj_set_style_radius(s_sleep_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_sleep_overlay, 0, 0);
    lv_obj_clear_flag(s_sleep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sleep_overlay, sleep_overlay_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_flag(s_sleep_overlay, LV_OBJ_FLAG_HIDDEN);

    idle_load();
    s_last_active_ms = esp_timer_get_time() / 1000;
    s_boot_ms = s_last_active_ms;

    /* 开机落在锁屏界面（不是直接进桌面） */
    do_lock_ex(true);

    statusbar_update();
    statusbar_wifi_update();

    lv_timer_create(shell_timer_cb, 30, NULL);

    /* 开机自检浮层（盖在锁屏之上，检查完自动淡出） */
    ui_boot_check_begin();

    lvgl_port_unlock();

    ESP_LOGI(TAG, "shell ready, %d app(s) registered", s_app_cnt);
    return ESP_OK;
}
