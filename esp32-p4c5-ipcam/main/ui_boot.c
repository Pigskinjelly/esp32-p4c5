/*
 * 开机自检浮层（盖在锁屏之上）
 *
 * 开机先转圈，逐项检查：系统 / 显示 / 触摸 / 内存 / 网络 / 相机 / 存储。
 * 每项结果由 main.c 在各初始化阶段上报；这里只负责显示。
 * 检查结束（或 8 秒超时）后停 1.2s，淡出露出锁屏。
 */
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "lvgl.h"
#include "ui_boot.h"
#include "ui_theme.h"
#include "ui_shell.h"
#include "wifi_mgr.h"

static const char *TAG = "ui_boot";

/* -1 未完成 / 0 失败 / 1 通过 */
static volatile int8_t s_state[BOOT_ITEM_CNT];
static char s_text[BOOT_ITEM_CNT][32];
static lv_obj_t *s_rows[BOOT_ITEM_CNT];

static lv_obj_t *s_root;
static lv_obj_t *s_spin;
static lv_obj_t *s_hint;
static lv_timer_t *s_timer;
static volatile bool s_done;
static int64_t s_start_ms;
static int64_t s_done_ms;
static int64_t s_done_at;

static const char *s_names[BOOT_ITEM_CNT] = {
    "系统", "显示", "触摸", "内存", "网络", "相机", "存储",
};

static void boot_spin_exec(void *obj, int32_t v);

static void boot_ui_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    if (s_spin) {
        lv_anim_delete(s_spin, boot_spin_exec);
    }
    if (s_root) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    s_spin = NULL;
    s_hint = NULL;
    for (int i = 0; i < BOOT_ITEM_CNT; i++) {
        s_rows[i] = NULL;
    }
}

static void boot_spin_exec(void *obj, int32_t v)
{
    lv_obj_t *arc = (lv_obj_t *)obj;
    int32_t start = v % 360;
    lv_arc_set_angles(arc, start, start + 90);
}

static void boot_fade_exec(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void boot_fade_done(lv_anim_t *a)
{
    (void)a;
    boot_ui_destroy();
    ESP_LOGI(TAG, "boot check overlay removed");
}

/* 定时把内存里的状态刷到界面（不依赖调用者的 LVGL 锁） */
static void boot_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_root) {
        return;
    }
    int fail = 0;
    int pending = 0;
    for (int i = 0; i < BOOT_ITEM_CNT; i++) {
        int8_t st = s_state[i];
        if (st == 0) {
            fail++;
        } else if (st == -1) {
            pending++;
        }
        if (!s_rows[i]) {
            continue;
        }
        char buf[64];
        const char *tag = (st == 1) ? "正常" : (st == 0) ? "异常" :
                          (st == -2) ? "跳过" : "检测中";
        if (s_text[i][0]) {
            snprintf(buf, sizeof(buf), "%.12s %.20s", s_names[i], s_text[i]);
        } else {
            snprintf(buf, sizeof(buf), "%.12s %.8s", s_names[i], tag);
        }
        lv_label_set_text(s_rows[i], buf);
        lv_obj_set_style_text_color(s_rows[i],
                                    lv_color_hex(st == 1 ? 0x37E8A0 :
                                                 st == 0 ? 0xFF8A8A :
                                                 st == -2 ? 0x8FA8CC : 0x5B6B85), 0);
    }

    int64_t now = esp_timer_get_time() / 1000;
    /* 检查项都结束了，但锁屏的时间/日期/天气还没出来 —— 等它，
     * 否则一揭开就是 "--:--" 和 "天气加载中"。
     * 最多等 30s（网络很差时也不能一直卡在自检界面）。 */
    if (s_done && s_done_ms == 0) {
        if (s_done_at == 0) {
            s_done_at = now;
        }
        if (!ui_shell_lock_ready()) {
            /* 自检开始时还没连上的，这里补记（否则一直显示"待连接"） */
            if (s_state[BOOT_WIFI] != 1 && wifi_mgr_get_state() == WIFI_MGR_CONNECTED) {
                ui_boot_check_mark(BOOT_WIFI, true, wifi_mgr_get_ip());
            }
            /* 从开机算总时长：WiFi 约 19s 连上 + 时间同步 2s + 天气 3s，
             * 给 35s 足够；射频关着就 8s 放行（根本来不了） */
            int64_t total = now - s_start_ms;
            int64_t deadline = wifi_mgr_is_on() ? 25000 : 8000;
            if (total < deadline) {
                if (s_hint) {
                    lv_label_set_text(s_hint, "正在同步时间与天气…");
                    lv_obj_set_style_text_color(s_hint, lv_color_hex(0x8FA8CC), 0);
                }
                return;
            }
            if (s_hint) {
                lv_label_set_text(s_hint, "网络较慢，先进系统");
            }
        }
    }
    if (s_done && s_done_ms == 0) {
        s_done_ms = now;
        ESP_LOGI(TAG, "boot overlay finishing at %lld ms (lock_ready=%d)",
                 (long long)(now - s_start_ms), (int)ui_shell_lock_ready());
        if (s_hint) {
            if (fail == 0) {
                lv_label_set_text(s_hint, "自检完成，一切正常");
                lv_obj_set_style_text_color(s_hint, lv_color_hex(0x37E8A0), 0);
            } else {
                lv_label_set_text_fmt(s_hint, "自检完成，%d 项异常", fail);
                lv_obj_set_style_text_color(s_hint, lv_color_hex(0xFFB060), 0);
            }
        }
    }

    /* 超时兜底：8 秒还没结束就收尾，别一直挡着 */
    if (!s_done && now - s_start_ms > 8000) {
        s_done = true;
        if (s_hint) {
            lv_label_set_text(s_hint, pending > 0 ? "自检超时，先进系统" : "自检完成");
        }
    }

    if (s_done && s_done_ms > 0 && now - s_done_ms > 1300) {
        if (s_spin) {
            lv_obj_add_flag(s_spin, LV_OBJ_FLAG_HIDDEN);
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_root);
        lv_anim_set_values(&a, 255, 0);
        lv_anim_set_duration(&a, 320);
        lv_anim_set_exec_cb(&a, boot_fade_exec);
        lv_anim_set_completed_cb(&a, boot_fade_done);
        lv_anim_start(&a);
        lv_timer_del(s_timer);
        s_timer = NULL;     /* 动画期间不再刷新 */
    }
}

void ui_boot_check_begin(void)
{
    s_start_ms = esp_timer_get_time() / 1000;
    s_done = false;
    s_done_ms = 0;
    s_done_at = 0;
    for (int i = 0; i < BOOT_ITEM_CNT; i++) {
        s_state[i] = -1;
        s_text[i][0] = 0;
        s_rows[i] = NULL;
    }

    s_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_root, 480, 480);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, col(0x05080F), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_root);

    lv_obj_t *title = ui_label(s_root, "系统自检", &ui_cjk_16, 0xDCE6F2);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *sub = ui_label(s_root, "ESP32-P4C5", &lv_font_montserrat_14, 0x5B6B85);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 66);

    /* 转圈动画：自己算角度，动画步长小、只刷圆弧，比 lv_spinner 顺 */
    s_spin = lv_arc_create(s_root);
    lv_obj_set_size(s_spin, 76, 76);
    lv_obj_align(s_spin, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_remove_style(s_spin, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_spin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_spin, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_bg_angles(s_spin, 0, 360);
    lv_arc_set_angles(s_spin, 0, 90);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(0x1D3C74), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_spin, 7, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_spin, true, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_spin, lv_color_hex(0x37D8E8), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_spin, 7, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_spin, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_spin, LV_OPA_COVER, LV_PART_INDICATOR);
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_spin);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_duration(&a, 1500);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_set_exec_cb(&a, boot_spin_exec);
        lv_anim_start(&a);
    }

    for (int i = 0; i < BOOT_ITEM_CNT; i++) {
        int cx = i % 2;
        int ry = i / 2;
        lv_obj_t *l = ui_label(s_root, "", &ui_cjk_16, 0x5B6B85);
        lv_obj_set_pos(l, 40 + cx * 212, 206 + ry * 34);
        s_rows[i] = l;
    }

    s_hint = ui_label(s_root, "正在检查…", &ui_cjk_16, 0x7FA8D8);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -54);

    s_timer = lv_timer_create(boot_tick_cb, 120, NULL);

    ESP_LOGI(TAG, "boot check UI ready");
}

void ui_boot_check_mark(int item, bool ok, const char *text)
{
    if (item < 0 || item >= BOOT_ITEM_CNT) {
        return;
    }
    s_state[item] = ok ? 1 : 0;
    if (text && text[0]) {
        strlcpy(s_text[item], text, sizeof(s_text[item]));
    }
    ESP_LOGI(TAG, "[check] %s = %s (%s)", s_names[item], ok ? "OK" : "FAIL",
             text ? text : "");
}

void ui_boot_check_skip(int item, const char *text)
{
    if (item < 0 || item >= BOOT_ITEM_CNT) {
        return;
    }
    s_state[item] = -2;
    if (text && text[0]) {
        strlcpy(s_text[item], text, sizeof(s_text[item]));
    }
    ESP_LOGI(TAG, "[check] %s = SKIP (%s)", s_names[item], text ? text : "");
}

void ui_boot_check_done(void)
{
    s_done = true;
}
