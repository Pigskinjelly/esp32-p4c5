/*
 * 设置 App —— 系统状态 / 亮度 / 重启
 */
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "app_audio.h"
#include "app_video.h"
#include "wifi_mgr.h"
#include "app_storage.h"
#include "bt_mgr.h"
#include "stock_cfg.h"
#include "esp_app_desc.h"

static lv_obj_t *s_lbl_wifi;
static lv_obj_t *s_lbl_storage;
static lv_obj_t *s_lbl_mem;
static lv_obj_t *s_lbl_uptime;
static lv_obj_t *s_slider;
static lv_obj_t *s_vol_lbl;
static lv_obj_t *s_manage;      /* 应用管理浮层 */
static bool s_sw_sync;
static void app_bg_switch_cb(lv_event_t *e);

static void volume_cb(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    app_audio_set_volume(v);
    if (s_vol_lbl) {
        lv_label_set_text_fmt(s_vol_lbl, "%d", v);
    }
}
static lv_obj_t *s_sw_bt;
static lv_obj_t *s_idle_btns[5];
static lv_obj_t *s_rot_btns[4];

/* 待机时间选项（秒，0=永不） */
static const int s_idle_vals[5] = { 30, 60, 120, 300, 0 };

/* ---- 相机分辨率 / 帧率 ---- */
static lv_obj_t *s_res_btns[2];
static lv_obj_t *s_fps_btns[2];
/* 1080p 实测会让 ISP fifo 溢出（MIPI/ISP 带宽不够），只提供这两档 */
static const int s_res_w[2] = { 1280, 800 };
static const int s_res_h[2] = { 720, 800 };

static void cam_cfg_refresh(void)
{
    int cw, ch, cf;
    app_video_capture_cfg_get(&cw, &ch, &cf);
    for (int i = 0; i < 2; i++) {
        if (!s_res_btns[i]) {
            continue;
        }
        bool on = (cw == s_res_w[i] && ch == s_res_h[i]);
        lv_obj_set_style_bg_color(s_res_btns[i], col(on ? 0x2E4C7A : 0x16203A), 0);
    }
    for (int i = 0; i < 2; i++) {
        if (!s_fps_btns[i]) {
            continue;
        }
        bool on = (cf == (i ? 60 : 30));
        lv_obj_set_style_bg_color(s_fps_btns[i], col(on ? 0x2E4C7A : 0x16203A), 0);
    }
}

static void res_btn_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i > 1) {
        return;
    }
    int cw, ch, cf;
    app_video_capture_cfg_get(&cw, &ch, &cf);
    if (s_res_w[i] != 1280) {
        cf = 30;                        /* 只有 720p 有 60fps */
    }
    app_video_capture_cfg_set(s_res_w[i], s_res_h[i], cf);
    cam_cfg_refresh();
    ui_shell_toast("重启后生效");
}

static void fps_btn_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int cw, ch, cf;
    app_video_capture_cfg_get(&cw, &ch, &cf);
    if (i == 1 && cw != 1280) {
        ui_shell_toast("60 帧只在 720P 下支持");
        return;
    }
    app_video_capture_cfg_set(cw, ch, i ? 60 : 30);
    cam_cfg_refresh();
    ui_shell_toast("重启后生效");
}

static void rot_btn_refresh(void)
{
    ui_rot_t cur = ui_shell_get_rotation();
    for (int i = 0; i < 4; i++) {
        if (!s_rot_btns[i]) {
            continue;
        }
        bool on = (i == (int)cur);
        lv_obj_set_style_bg_color(s_rot_btns[i], col(on ? C_ACCENT : 0x1D3C74), 0);
    }
}

static void rot_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 3) {
        return;
    }
    ui_shell_set_rotation((ui_rot_t)idx);
    rot_btn_refresh();
    ui_shell_toast("显示方向已切换");
}

static void bt_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    esp_err_t err = bt_mgr_set(on);
    if (err != ESP_OK) {
        ui_shell_toast(bt_mgr_available() ? "蓝牙切换失败" : "固件未编译蓝牙");
        if (on) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        return;
    }
    ui_shell_toast(on ? "蓝牙已开启" : "蓝牙已关闭");
}

static void idle_btn_refresh(void)
{
    int cur = ui_shell_get_idle_timeout();
    for (int i = 0; i < 5; i++) {
        bool on = (s_idle_vals[i] == cur);
        lv_obj_set_style_bg_color(s_idle_btns[i], col(on ? C_ACCENT : 0x1D3C74), 0);
    }
}

static void idle_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 4) {
        return;
    }
    ui_shell_set_idle_timeout(s_idle_vals[idx]);
    idle_btn_refresh();
    ui_shell_toast(s_idle_vals[idx] == 0 ? "已设为永不息屏" : "待机时间已保存");
}

/* 格式化存储：必须点两次（5 秒内），防误触；TF 卡在位时拒绝 */
static int64_t s_fmt_arm_ms;

static void storage_format_cb(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time() / 1000;
    if (s_fmt_arm_ms && now - s_fmt_arm_ms < 5000) {
        s_fmt_arm_ms = 0;
        if (app_storage_is_sd()) {
            ui_shell_toast("照片在 TF 卡上，不格式化");
            return;
        }
        esp_err_t err = app_storage_format();
        ui_shell_toast(err == ESP_OK ? "存储已格式化" : "格式化失败");
        return;
    }
    s_fmt_arm_ms = now;
    ui_shell_toast("再点一次确认格式化（5 秒内）");
}

static void reboot_cb(lv_event_t *e)
{
    (void)e;
    ui_shell_toast("3 秒后重启…");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

/* ---- 应用管理：每个 App 一个开关（关掉就不显示、不创建 UI、不跑后台） ---- */

static void app_switch_cb(lv_event_t *e)
{
    if (s_sw_sync) {
        return;
    }
    lv_obj_t *sw = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_shell_set_app_enabled(idx, on);
    /* 允许被"拒绝"的情况（设置自身不能关）：把开关状态拉回真值 */
    if (ui_shell_app_enabled(idx) != on) {
        s_sw_sync = true;
        if (on) {
            lv_obj_clear_state(sw, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }
        s_sw_sync = false;
    }
    ui_shell_toast(on ? "已启用，桌面已更新" : "已关闭，桌面已移除");
}

static void manage_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_manage) {
        lv_obj_add_flag(s_manage, LV_OBJ_FLAG_HIDDEN);
    }
}

static void manage_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_manage) {
        lv_obj_remove_flag(s_manage, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_manage);
    }
}

/* ---- 交易 App 的"应用管理"后台：前台/后台刷新率 ---- */

static lv_obj_t *s_stk;                 /* 交易设置浮层 */
static lv_obj_t *s_stk_fb[3];
static lv_obj_t *s_stk_bb[3];
static const int s_fvals[3] = { 1, 3, 5 };
static const int s_bvals[3] = { 3, 5, 10 };

static void stk_refresh(void)
{
    int f = stock_cfg_front();
    int b = stock_cfg_bg();
    for (int i = 0; i < 3; i++) {
        if (s_stk_fb[i]) {
            lv_obj_set_style_bg_color(s_stk_fb[i],
                                      col(s_fvals[i] == f ? C_ACCENT : 0x16203A), 0);
        }
        if (s_stk_bb[i]) {
            lv_obj_set_style_bg_color(s_stk_bb[i],
                                      col(s_bvals[i] == b ? C_ACCENT : 0x16203A), 0);
        }
    }
}

static void stk_f_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i > 2) {
        return;
    }
    stock_cfg_set_front(s_fvals[i]);
    stk_refresh();
    char b[48];
    snprintf(b, sizeof(b), "行情每 %d 秒刷新", s_fvals[i]);
    ui_shell_toast(b);
}

static void stk_b_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i > 2) {
        return;
    }
    stock_cfg_set_bg(s_bvals[i]);
    stk_refresh();
    char b[48];
    snprintf(b, sizeof(b), "后台每 %d 秒拉一次", s_bvals[i]);
    ui_shell_toast(b);
}

static void stk_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_stk) {
        lv_obj_add_flag(s_stk, LV_OBJ_FLAG_HIDDEN);
    }
}

/* 直接点「交易」这一行也能进后台（子控件的冒泡忽略掉，
 * 否则点开关也会误开这一页） */
static void row_trade_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;
    }
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || strcmp(ui_shell_app_name(i), "交易") != 0) {
        return;
    }
    if (s_stk) {
        stk_refresh();
        lv_obj_remove_flag(s_stk, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_stk);
    }
}

static void stk_open_cb(lv_event_t *e)
{
    (void)e;
    if (s_stk) {
        stk_refresh();
        lv_obj_remove_flag(s_stk, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_stk);
    }
}

/* 一行三个选项按钮 */
static void stk_btn_row(lv_obj_t *parent, int y, const int *vals, lv_obj_t **store,
                        const char *const *txt, lv_event_cb_t cb)
{
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(parent);
        lv_obj_set_size(b, 140, 40);
        lv_obj_set_pos(b, 10 + i * 148, y);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, col(0x16203A), 0);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, txt[i], &ui_cjk_16, C_TXT);
        lv_obj_center(l);
        store[i] = b;
    }
    (void)vals;
}

static void build_stk_cfg(lv_obj_t *parent)
{
    s_stk = lv_obj_create(parent);
    lv_obj_set_size(s_stk, 480, 480);
    lv_obj_set_pos(s_stk, 0, 0);
    lv_obj_set_style_bg_color(s_stk, col(C_BG), 0);
    lv_obj_set_style_bg_opa(s_stk, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_stk, 0, 0);
    lv_obj_set_style_radius(s_stk, 0, 0);
    lv_obj_set_style_pad_all(s_stk, 0, 0);
    lv_obj_clear_flag(s_stk, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_stk, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = ui_label(s_stk, "交易 · 应用管理", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(title, 16, 10);

    lv_obj_t *back = lv_button_create(s_stk);
    lv_obj_set_size(back, 84, 32);
    lv_obj_set_pos(back, 380, 6);
    lv_obj_set_style_radius(back, 16, 0);
    lv_obj_set_style_bg_color(back, col(0x2E4C7A), 0);
    lv_obj_add_event_cb(back, stk_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(back, "返回", &ui_cjk_16, C_TXT);
    lv_obj_center(bl);

    /* 1. App 内行情刷新率 */
    lv_obj_t *c1 = ui_card(s_stk, 456, 120, C_BG_CARD);
    lv_obj_set_pos(c1, 12, 52);
    lv_obj_t *t1 = ui_label(c1, "行情刷新", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(t1, 10, 8);
    lv_obj_t *s1 = ui_label(c1, "价格 / 涨跌幅 / 盘口的刷新间隔", &ui_cjk_16, 0x5B6B85);
    lv_obj_set_pos(s1, 10, 32);
    static const char *ftxt[3] = { "每秒刷新", "3 秒刷新", "5 秒刷新" };
    stk_btn_row(c1, 68, s_fvals, s_stk_fb, ftxt, stk_f_cb);

    /* 2. 后台刷新率（价格提醒弹窗的快慢） */
    lv_obj_t *c2 = ui_card(s_stk, 456, 152, C_BG_CARD);
    lv_obj_set_pos(c2, 12, 182);
    lv_obj_t *t2 = ui_label(c2, "后台刷新", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(t2, 10, 8);
    lv_obj_t *s2 = ui_label(c2, "退出 App 后仍拉行情，决定提醒弹窗的快慢", &ui_cjk_16, 0x5B6B85);
    lv_obj_set_pos(s2, 10, 32);
    static const char *btxt[3] = { "3 秒", "5 秒", "10 秒" };
    stk_btn_row(c2, 62, s_bvals, s_stk_bb, btxt, stk_b_cb);
    lv_obj_t *h2 = ui_label(c2, "需要在上层列表里给「交易」打开「后台」", &ui_cjk_16, 0xE8A537);
    lv_obj_set_pos(h2, 10, 116);

    /* 3. K 线操作说明 */
    lv_obj_t *c3 = ui_card(s_stk, 456, 128, C_BG_CARD);
    lv_obj_set_pos(c3, 12, 344);
    lv_obj_t *t3 = ui_label(c3, "日 K 操作", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(t3, 10, 8);
    lv_obj_t *l1 = ui_label(c3, "按住 K 线 → 看那一天的详细数据", &ui_cjk_16, 0x8FA8CC);
    lv_obj_set_pos(l1, 10, 38);
    lv_obj_t *l2 = ui_label(c3, "左右拖动 → 翻看更早的历史", &ui_cjk_16, 0x8FA8CC);
    lv_obj_set_pos(l2, 10, 66);
    lv_obj_t *l3 = ui_label(c3, "松手 → 回到现在的实时行情", &ui_cjk_16, 0x8FA8CC);
    lv_obj_set_pos(l3, 10, 94);
}

static void build_manage(lv_obj_t *parent)
{
    s_manage = lv_obj_create(parent);
    lv_obj_set_size(s_manage, 480, 480);
    lv_obj_set_pos(s_manage, 0, 0);
    lv_obj_set_style_bg_color(s_manage, col(C_BG), 0);
    lv_obj_set_style_bg_opa(s_manage, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_manage, 0, 0);
    lv_obj_set_style_radius(s_manage, 0, 0);
    lv_obj_set_style_pad_all(s_manage, 0, 0);
    lv_obj_clear_flag(s_manage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_manage, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = ui_label(s_manage, "应用管理", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(title, 16, 10);

    lv_obj_t *back = lv_button_create(s_manage);
    lv_obj_set_size(back, 84, 32);
    lv_obj_set_pos(back, 380, 6);
    lv_obj_set_style_radius(back, 16, 0);
    lv_obj_set_style_bg_color(back, col(0x2E4C7A), 0);
    lv_obj_add_event_cb(back, manage_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(back, "返回", &ui_cjk_16, C_TXT);
    lv_obj_center(bl);

    lv_obj_t *tip = ui_label(s_manage, "运行=桌面显示；后台=退出后保留（关掉更省内存）",
                             &ui_cjk_16, 0x5B6B85);
    lv_obj_set_pos(tip, 16, 44);

    lv_obj_t *list = lv_obj_create(s_manage);
    lv_obj_set_size(list, 456, 380);
    lv_obj_set_pos(list, 12, 74);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_radius(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    int n = ui_shell_app_count();
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, 448, 50);
        lv_obj_set_style_bg_color(row, col(C_BG_CARD), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = ui_label(row, ui_shell_app_name(i), &ui_cjk_16, C_TXT);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t *l1 = ui_label(row, "运行", &ui_cjk_16, 0x7B8CA6);
        lv_obj_align(l1, LV_ALIGN_LEFT_MID, 150, 0);
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 52, 26);
        lv_obj_align(sw, LV_ALIGN_LEFT_MID, 192, 0);
        lv_obj_set_style_bg_color(sw, col(0x1D3C74), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, col(C_ACCENT), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sw, col(C_TXT), LV_PART_KNOB);
        lv_obj_add_event_cb(sw, app_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        if (ui_shell_app_enabled(i)) {
            lv_obj_add_state(sw, LV_STATE_CHECKED);
        }

        /* 后台开关：关掉后退出 App 会销毁界面，避免开多 App 卡顿 */
        lv_obj_t *l2 = ui_label(row, "后台", &ui_cjk_16, 0x7B8CA6);
        lv_obj_align(l2, LV_ALIGN_LEFT_MID, 258, 0);
        lv_obj_t *bw = lv_switch_create(row);
        lv_obj_set_size(bw, 52, 26);
        lv_obj_align(bw, LV_ALIGN_LEFT_MID, 300, 0);
        lv_obj_set_style_bg_color(bw, col(0x1D3C74), LV_PART_MAIN);
        lv_obj_set_style_bg_color(bw, col(0x8A6A2E), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bw, col(C_TXT), LV_PART_KNOB);
        lv_obj_add_event_cb(bw, app_bg_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
        if (ui_shell_app_bg(i)) {
            lv_obj_add_state(bw, LV_STATE_CHECKED);
        }

        /* 交易有独立后台：点这里进去调刷新率 */
        if (strcmp(ui_shell_app_name(i), "交易") == 0) {
            lv_obj_t *go = lv_button_create(row);
            lv_obj_set_size(go, 80, 30);
            lv_obj_set_pos(go, 360, 10);
            lv_obj_set_style_radius(go, 8, 0);
            lv_obj_set_style_pad_all(go, 0, 0);
            lv_obj_set_style_bg_color(go, col(0x2E4C7A), 0);
            lv_obj_add_event_cb(go, stk_open_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *gl = ui_label(go, "刷新率 >", &ui_cjk_16, C_TXT);
            lv_obj_center(gl);
            lv_obj_add_event_cb(row, row_trade_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }

        /* 设置自己不能关，否则进不来 */
        if (strcmp(ui_shell_app_name(i), "设置") == 0) {
            lv_obj_add_state(sw, LV_STATE_DISABLED);
            lv_obj_set_style_opa(sw, LV_OPA_50, 0);
        }
    }
}

static void app_bg_switch_cb(lv_event_t *e)
{
    if (s_sw_sync) {
        return;
    }
    lv_obj_t *sw = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    ui_shell_app_bg_set(idx, on);
    ui_shell_toast(on ? "已允许后台，退出后继续保留" : "退出即释放，省内存");
}

static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    ui_shell_set_brightness(lv_slider_get_value(slider));
}

static lv_obj_t *add_row(lv_obj_t *parent, int y, const char *key)
{
    lv_obj_t *card = ui_card(parent, 456, 52, C_BG_CARD);
    lv_obj_set_pos(card, 12, y);
    lv_obj_t *k = ui_label(card, key, NULL, C_TXT_DIM);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 4, 0);
    return card;
}

/* 每次重建界面前清空控件指针。
 * App 没开"后台"时退出会销毁整个界面，静态指针就此悬空；
 * 下一次 create 里若还有函数先用到它（比如按旧按钮改样式），
 * 就会 Load access fault 直接重启。 */
static void reset_ui_refs(void)
{
    s_lbl_wifi = NULL;
    s_lbl_storage = NULL;
    s_lbl_mem = NULL;
    s_lbl_uptime = NULL;
    s_slider = NULL;
    s_vol_lbl = NULL;
    s_manage = NULL;
    s_sw_bt = NULL;
    s_stk = NULL;
    memset(s_stk_fb, 0, sizeof(s_stk_fb));
    memset(s_stk_bb, 0, sizeof(s_stk_bb));
    memset(s_idle_btns, 0, sizeof(s_idle_btns));
    memset(s_rot_btns, 0, sizeof(s_rot_btns));
    memset(s_res_btns, 0, sizeof(s_res_btns));
    memset(s_fps_btns, 0, sizeof(s_fps_btns));
}

static void create(lv_obj_t *parent)
{
    reset_ui_refs();
    ui_app_topbar(parent, "设置");

    lv_obj_t *r;
    /* 一屏放不下：内容放进可上下滑动的容器（原来固定位置，超出屏幕就看不到） */
    lv_obj_t *sc = lv_obj_create(parent);
    lv_obj_set_size(sc, 480, 480);
    lv_obj_set_pos(sc, 0, 0);
    lv_obj_set_style_bg_opa(sc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sc, 0, 0);
    lv_obj_set_style_radius(sc, 0, 0);
    lv_obj_set_style_pad_all(sc, 0, 0);
    lv_obj_set_scroll_dir(sc, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sc, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_snap_y(sc, LV_SCROLL_SNAP_NONE);
    lv_obj_set_style_width(sc, 8, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(sc, col(0x2E4C7A), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(sc, 180, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(sc, 4, LV_PART_SCROLLBAR);

    int y = 10;
    const int step = 44;
    const int h = 40;

    r = add_row(sc, y, "Wi-Fi");
    lv_obj_set_size(r, 456, h);
    s_lbl_wifi = ui_label(r, "--", &ui_cjk_16, C_TXT);
    lv_obj_align(s_lbl_wifi, LV_ALIGN_RIGHT_MID, -4, 0);
    y += step;

    r = add_row(sc, y, "存储");
    lv_obj_set_size(r, 456, h);
    s_lbl_storage = ui_label(r, "--", &ui_cjk_16, C_TXT);
    lv_obj_align(s_lbl_storage, LV_ALIGN_RIGHT_MID, -4, 0);
    y += step;

    /* 存储维护：flash 分区坏了不会自动格式化（R-09），这里给手动入口 */
    {
        lv_obj_t *fr = lv_button_create(sc);
        lv_obj_set_size(fr, 456, h);
        lv_obj_set_pos(fr, 12, y);
        lv_obj_set_style_bg_color(fr, col(C_BG_CARD), 0);
        lv_obj_set_style_radius(fr, 12, 0);
        lv_obj_set_style_pad_all(fr, 0, 0);
        lv_obj_add_event_cb(fr, storage_format_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *fk = ui_label(fr, "格式化存储", &ui_cjk_16, C_TXT_DIM);
        lv_obj_align(fk, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *fv = ui_label(fr, "连点两次", &ui_cjk_16, 0x7FA8D8);
        lv_obj_align(fv, LV_ALIGN_RIGHT_MID, -12, 0);
        y += step;
    }

    r = add_row(sc, y, "内存");
    lv_obj_set_size(r, 456, h);
    s_lbl_mem = ui_label(r, "--", &ui_cjk_16, C_TXT);
    lv_obj_align(s_lbl_mem, LV_ALIGN_RIGHT_MID, -4, 0);
    y += step;

    r = add_row(sc, y, "运行时间");
    lv_obj_set_size(r, 456, h);
    s_lbl_uptime = ui_label(r, "--", &ui_cjk_16, C_TXT);
    lv_obj_align(s_lbl_uptime, LV_ALIGN_RIGHT_MID, -4, 0);
    y += step;

    /* 亮度 */
    lv_obj_t *br = ui_card(sc, 456, h, C_BG_CARD);
    lv_obj_set_pos(br, 12, y);
    lv_obj_t *bk = ui_label(br, "亮度", &ui_cjk_16, C_TXT_DIM);
    lv_obj_align(bk, LV_ALIGN_LEFT_MID, 4, 0);
    s_slider = lv_slider_create(br);
    lv_obj_set_size(s_slider, 280, 16);
    lv_obj_align(s_slider, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_slider_set_range(s_slider, 5, 100);
    lv_slider_set_value(s_slider, ui_shell_get_brightness(), LV_ANIM_OFF);
    lv_obj_add_event_cb(s_slider, brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(s_slider, col(0x1D3C74), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, col(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, col(C_TXT), LV_PART_KNOB);
    y += step;

    /* 音量 */
    lv_obj_t *vol = ui_card(sc, 456, h, C_BG_CARD);
    lv_obj_set_pos(vol, 12, y);
    lv_obj_t *vk = ui_label(vol, "音量", &ui_cjk_16, C_TXT_DIM);
    lv_obj_align(vk, LV_ALIGN_LEFT_MID, 4, 0);
    s_vol_lbl = ui_label(vol, "0", &lv_font_montserrat_14, C_TXT_DIM);
    lv_obj_align(s_vol_lbl, LV_ALIGN_LEFT_MID, 46, 0);
    lv_obj_t *vs = lv_slider_create(vol);
    lv_obj_set_size(vs, 280, 16);
    lv_obj_align(vs, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_slider_set_range(vs, 0, 100);
    /* 显示真实音量（以前这里写死 "60"，重启后实际是 0 也显示 60） */
    lv_slider_set_value(vs, app_audio_get_volume(), LV_ANIM_OFF);
    lv_label_set_text_fmt(s_vol_lbl, "%d", app_audio_get_volume());
    lv_obj_add_event_cb(vs, volume_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_style_bg_color(vs, col(0x1D3C74), LV_PART_MAIN);
    lv_obj_set_style_bg_color(vs, col(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vs, col(C_TXT), LV_PART_KNOB);
    y += step;

    /* 待机时间（无操作多久自动进锁屏） */
    lv_obj_t *idle = ui_card(sc, 456, h, C_BG_CARD);
    lv_obj_set_pos(idle, 12, y);
    lv_obj_t *ik = ui_label(idle, "待机", &ui_cjk_16, C_TXT_DIM);
    lv_obj_align(ik, LV_ALIGN_LEFT_MID, 4, 0);

    static const char *idle_txt[5] = { "30秒", "1分", "2分", "5分", "永不" };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = lv_button_create(idle);
        lv_obj_set_size(b, 64, 30);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -4 - (4 - i) * 68, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, idle_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, idle_txt[i], &ui_cjk_16, C_TXT);
        lv_obj_center(l);
        s_idle_btns[i] = b;
    }
    idle_btn_refresh();
    y += step;

    /* 蓝牙开关 */
    lv_obj_t *bt = ui_card(sc, 456, h, C_BG_CARD);
    lv_obj_set_pos(bt, 12, y);
    lv_obj_t *btk = ui_label(bt, "蓝牙", NULL, C_TXT_DIM);
    lv_obj_align(btk, LV_ALIGN_LEFT_MID, 4, 0);
    s_sw_bt = lv_switch_create(bt);
    lv_obj_set_size(s_sw_bt, 60, 28);
    lv_obj_align(s_sw_bt, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(s_sw_bt, col(0x1D3C74), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_sw_bt, col(C_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sw_bt, col(C_TXT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_sw_bt, bt_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (bt_mgr_is_on()) {
        lv_obj_add_state(s_sw_bt, LV_STATE_CHECKED);
    }
    if (!bt_mgr_available()) {
        lv_obj_add_state(s_sw_bt, LV_STATE_DISABLED);
        lv_obj_set_style_opa(s_sw_bt, LV_OPA_40, 0);
    }
    y += step;

    /* 显示方向：横 / 纵 / 横反 / 纵反 —— 整个 UI 一起转 */
    lv_obj_t *rot = ui_card(sc, 456, h, C_BG_CARD);
    lv_obj_set_pos(rot, 12, y);
    lv_obj_t *rk = ui_label(rot, "方向", NULL, C_TXT_DIM);
    lv_obj_align(rk, LV_ALIGN_LEFT_MID, 4, 0);
    static const char *rot_txt[4] = { "横向", "纵向", "横反", "纵反" };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(rot);
        lv_obj_set_size(b, 58, 30);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -4 - (3 - i) * 62, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, rot_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, rot_txt[i], NULL, C_TXT);
        lv_obj_center(l);
        s_rot_btns[i] = b;
    }
    rot_btn_refresh();
    y += step;

    /* 相机分辨率（重启后生效） */
    {
        lv_obj_t *rc = ui_card(sc, 456, h, C_BG_CARD);
        lv_obj_set_pos(rc, 12, y);
        lv_obj_t *lk = ui_label(rc, "分辨率", NULL, C_TXT_DIM);
        lv_obj_align(lk, LV_ALIGN_LEFT_MID, 4, 0);
        static const char *rt[2] = { "720P", "800方" };
        for (int i = 0; i < 2; i++) {
            lv_obj_t *b = lv_button_create(rc);
            lv_obj_set_size(b, 76, 30);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -4 - (1 - i) * 82, 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_set_style_bg_color(b, col(0x16203A), 0);
            lv_obj_add_event_cb(b, res_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *l = ui_label(b, rt[i], NULL, C_TXT);
            lv_obj_center(l);
            s_res_btns[i] = b;
        }
        cam_cfg_refresh();
        y += step;
    }

    /* 相机帧率（重启后生效） */
    {
        lv_obj_t *fc = ui_card(sc, 456, h, C_BG_CARD);
        lv_obj_set_pos(fc, 12, y);
        lv_obj_t *lk = ui_label(fc, "帧率", NULL, C_TXT_DIM);
        lv_obj_align(lk, LV_ALIGN_LEFT_MID, 4, 0);
        static const char *ft[2] = { "30 帧", "60 帧" };
        for (int i = 0; i < 2; i++) {
            lv_obj_t *b = lv_button_create(fc);
            lv_obj_set_size(b, 86, 30);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -4 - (1 - i) * 92, 0);
            lv_obj_set_style_radius(b, 8, 0);
            lv_obj_set_style_pad_all(b, 0, 0);
            lv_obj_set_style_bg_color(b, col(0x16203A), 0);
            lv_obj_add_event_cb(b, fps_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *l = ui_label(b, ft[i], NULL, C_TXT);
            lv_obj_center(l);
            s_fps_btns[i] = b;
        }
        cam_cfg_refresh();
        y += step;
    }

    /* 应用管理入口 */
    lv_obj_t *app_row = lv_button_create(sc);
    lv_obj_set_size(app_row, 456, h);
    lv_obj_set_pos(app_row, 12, y);
    lv_obj_set_style_bg_color(app_row, col(C_BG_CARD), 0);
    lv_obj_set_style_radius(app_row, 12, 0);
    lv_obj_set_style_pad_all(app_row, 0, 0);
    lv_obj_add_event_cb(app_row, manage_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ak = ui_label(app_row, "应用管理", &ui_cjk_16, C_TXT_DIM);
    lv_obj_align(ak, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *av = ui_label(app_row, "开关 App  >", &ui_cjk_16, 0x7FA8D8);
    lv_obj_align(av, LV_ALIGN_RIGHT_MID, -12, 0);
    y += step;

    lv_obj_t *reboot = lv_button_create(sc);
    lv_obj_set_size(reboot, 200, 40);
    lv_obj_set_pos(reboot, 140, y + 6);
    lv_obj_set_style_bg_color(reboot, col(0x6B2038), 0);
    lv_obj_add_event_cb(reboot, reboot_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = ui_label(reboot, "重启设备", &ui_cjk_16, C_TXT);
    lv_obj_center(rl);

    build_manage(parent);
    build_stk_cfg(parent);
}

static void tick(void)
{
    wifi_mgr_state_t st = wifi_mgr_get_state();
    if (st == WIFI_MGR_CONNECTED) {
        lv_label_set_text_fmt(s_lbl_wifi, "%s  %s", wifi_mgr_get_ssid(), wifi_mgr_get_ip());
    } else {
        lv_label_set_text(s_lbl_wifi, wifi_mgr_state_str(st));
    }

    int used_kb, free_kb;
    app_storage_usage(&used_kb, &free_kb);
    lv_label_set_text_fmt(s_lbl_storage, "%s  剩余 %d MB",
                          app_storage_is_sd() ? "TF卡" : "Flash",
                          free_kb / 1024);

    uint32_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    uint32_t spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;
    lv_label_set_text_fmt(s_lbl_mem, "内部 %lu KB  PSRAM %lu KB",
                          (unsigned long)internal, (unsigned long)spiram);

    int64_t us = esp_timer_get_time();
    lv_label_set_text_fmt(s_lbl_uptime, "%lld 分 %lld 秒", us / 60000000LL, (us / 1000000LL) % 60);
}

static void on_show(void) { ui_shell_set_tick_cb(tick); }
static void on_hide(void)
{
    ui_shell_set_tick_cb(NULL);
    if (s_manage) {
        lv_obj_add_flag(s_manage, LV_OBJ_FLAG_HIDDEN);   /* 下次进来回主页面 */
    }
    if (s_stk) {
        lv_obj_add_flag(s_stk, LV_OBJ_FLAG_HIDDEN);
    }
}

const ui_app_t ui_app_settings = {
    .name = "设置",
    .symbol = LV_SYMBOL_SETTINGS,
    .color = 0x44598A,
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
    .always_on = true,
};
