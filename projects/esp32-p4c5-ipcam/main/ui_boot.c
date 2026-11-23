/*
 * 开机自检：逐项检查 + 结果汇总
 *
 * 界面（对齐设计稿 12-self-test）：
 *   标题「开机自检」            右上角 通过数/总数
 *   5 行检查项（圆点 + 名称 + 右侧实测数据）
 *   底部整宽状态条 + 结果文案 + 版本号 + 「跳过」
 *
 * 项与数据来源：
 *   相机 SC2336   app_video 打开 / 出图结果
 *   TF 卡         app_storage_mount 结果与容量
 *   WiFi 模块     wifi_mgr 连接状态与 SSID/IP
 *   RTSP 服务     rtsp_server_start 结果与端口
 *   内存          内部堆余量与 PSRAM
 *
 * 状态流转（boot_stage_t）：
 *   CHECKING ──各项结束──> ALL_PASS / PARTIAL_FAIL
 *            ├─8s 未结束──> TIMEOUT
 *            └─用户点跳过──> CANCELLED
 *   非 CHECKING 阶段先停 1.3s（全通过时还要等锁屏时间/天气就绪），然后淡出。
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

/* 每项结果：-1 未完成 / 0 失败 / 1 通过 / -2 跳过 */
static volatile int8_t s_state[BOOT_ITEM_CNT];
static char s_text[BOOT_ITEM_CNT][40];

static lv_obj_t *s_dots[BOOT_ITEM_CNT];
static lv_obj_t *s_vals[BOOT_ITEM_CNT];

static lv_obj_t *s_root;
static lv_obj_t *s_prog;
static lv_obj_t *s_bar;
static lv_obj_t *s_hint;
static lv_timer_t *s_timer;

static volatile bool s_done;      /* 各项都结束了（可能仍有失败项） */
static volatile bool s_cancel;    /* 用户点了跳过 */
static int64_t s_start_ms;
static int64_t s_done_ms;
static int64_t s_done_at;

static const char *s_names[BOOT_ITEM_CNT] = {
    "相机 SC2336", "TF 卡", "WiFi 模块", "RTSP 服务", "内存",
};

typedef enum {
    BS_CHECKING = 0,
    BS_ALL_PASS,
    BS_PARTIAL_FAIL,
    BS_TIMEOUT,
    BS_CANCELLED,
} boot_stage_t;

static void boot_fade_exec(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void boot_ui_destroy(void)
{
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    /* 关键：必须真正删掉整层。
     * 只把指针置 NULL 的话，这层 480x480 的透明浮层会一直盖在 layer_top 上，
     * 而且默认是 CLICKABLE —— 结果是整屏点击全部被它吃掉，表现为"触摸失灵"。
     * （上一轮重写时漏了这一句，这里补回并加了注释防止再犯） */
    if (s_root) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    memset(s_dots, 0, sizeof(s_dots));
    memset(s_vals, 0, sizeof(s_vals));
    s_prog = NULL;
    s_bar = NULL;
    s_hint = NULL;
}

static void boot_fade_done(lv_anim_t *a)
{
    (void)a;
    boot_ui_destroy();
    /* 有这条日志就说明整层真的删掉了；缺失则说明浮层还在，会吞触摸 */
    ESP_LOGI(TAG, "boot overlay removed");
}

static uint32_t stage_color(boot_stage_t st)
{
    switch (st) {
    case BS_ALL_PASS:     return 0x37E8A0;   /* 绿：全部通过 */
    case BS_PARTIAL_FAIL: return 0xFF8A8A;   /* 红：有失败项 */
    case BS_TIMEOUT:      return 0xFFB060;   /* 橙：超时 */
    case BS_CANCELLED:    return 0x8FA8CC;   /* 灰蓝：用户跳过 */
    default:              return 0x22D3EE;   /* 青：进行中 */
    }
}

static const char *stage_text(boot_stage_t st, int pass, int fail)
{
    static char buf[72];
    switch (st) {
    case BS_ALL_PASS:
        return "自检完成，正在进入桌面";
    case BS_PARTIAL_FAIL:
        snprintf(buf, sizeof(buf), "%d 项未通过，请在设置里检查", fail);
        return buf;
    case BS_TIMEOUT:
        snprintf(buf, sizeof(buf), "自检超时（%d/%d），先进系统", pass, BOOT_ITEM_CNT);
        return buf;
    case BS_CANCELLED:
        return "已跳过自检，正在进入桌面";
    default:
        snprintf(buf, sizeof(buf), "正在自检 %d/%d …", pass, BOOT_ITEM_CNT);
        return buf;
    }
}

static void boot_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_root) {
        return;
    }

    int pass = 0, fail = 0;
    for (int i = 0; i < BOOT_ITEM_CNT; i++) {
        int8_t st = s_state[i];
        if (st == 1) {
            pass++;
        } else if (st == 0) {
            fail++;
        }
        if (!s_vals[i]) {
            continue;
        }
        const char *tag = (st == 1) ? "正常" : (st == 0) ? "异常" :
                          (st == -2) ? "跳过" : "检测中";
        lv_label_set_text(s_vals[i], s_text[i][0] ? s_text[i] : tag);
        lv_obj_set_style_text_color(s_vals[i],
                                    lv_color_hex(st == 1 ? 0x9FE1CB :
                                                 st == 0 ? 0xFF8A8A :
                                                 st == -2 ? 0x6B7F9E : 0x5B6B85), 0);
        if (s_dots[i]) {
            lv_obj_set_style_bg_color(s_dots[i],
                                      lv_color_hex(st == 1 ? 0x37E8A0 :
                                                   st == 0 ? 0xFF8A8A : 0x39496B), 0);
        }
    }

    if (s_prog) {
        lv_label_set_text_fmt(s_prog, "%d/%d", pass, BOOT_ITEM_CNT);
    }

    int64_t now = esp_timer_get_time() / 1000;

    /* ---- 阶段判定：只在这里算，避免状态散落 ---- */
    boot_stage_t stage;
    if (s_cancel) {
        stage = BS_CANCELLED;
    } else if (s_done) {
        stage = (fail > 0) ? BS_PARTIAL_FAIL : BS_ALL_PASS;
    } else if (now - s_start_ms > 8000) {
        stage = BS_TIMEOUT;
        s_done = true;
    } else {
        stage = BS_CHECKING;
    }

    if (s_bar) {
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(stage_color(stage)), 0);
    }
    if (s_hint) {
        lv_label_set_text(s_hint, stage_text(stage, pass, fail));
        lv_obj_set_style_text_color(s_hint,
                                    lv_color_hex(stage == BS_CHECKING ? 0x8FA8CC
                                                                      : stage_color(stage)), 0);
    }

    /* ---- 收尾：等锁屏的时间/天气就绪再揭开 ---- */
    if (stage != BS_CHECKING && s_done_ms == 0) {
        if (s_done_at == 0) {
            s_done_at = now;
        }
        bool wait_lock = (stage == BS_ALL_PASS);   /* 只有全通过才值得等天气 */
        if (wait_lock && !ui_shell_lock_ready()) {
            /* 自检期间还没连上的，这里补记，避免一直显示"待连接" */
            if (s_state[BOOT_WIFI] != 1 && wifi_mgr_get_state() == WIFI_MGR_CONNECTED) {
                ui_boot_check_mark(BOOT_WIFI, true, wifi_mgr_get_ip());
            }
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
        s_done_ms = now;
        ESP_LOGI(TAG, "boot overlay finishing at %lld ms (stage=%d fail=%d lock_ready=%d)",
                 (long long)(now - s_start_ms), (int)stage, fail,
                 (int)ui_shell_lock_ready());
    }

    if (s_done_ms > 0 && now - s_done_ms > 1300) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_root);
        lv_anim_set_values(&a, 255, 0);
        lv_anim_set_duration(&a, 320);
        lv_anim_set_exec_cb(&a, boot_fade_exec);
        lv_anim_set_completed_cb(&a, boot_fade_done);
        lv_anim_start(&a);
        lv_timer_del(s_timer);
        s_timer = NULL;     /* 动画期间不再刷新，避免访问已释放对象 */
    }
}

static void boot_skip_cb(lv_event_t *e)
{
    (void)e;
    if (s_cancel) {
        return;
    }
    s_cancel = true;
    ESP_LOGI(TAG, "user skipped self-test");
}

void ui_boot_check_begin(void)
{
    if (s_root) {
        return;                 /* 幂等：重复调用不重建 */
    }
    for (int i = 0; i < BOOT_ITEM_CNT; i++) {
        s_state[i] = -1;
        s_text[i][0] = 0;
        s_dots[i] = NULL;
        s_vals[i] = NULL;
    }
    s_done = false;
    s_cancel = false;
    s_done_ms = 0;
    s_done_at = 0;
    s_start_ms = esp_timer_get_time() / 1000;

    s_root = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_root, 480, 480);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, col(0x0B1220), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_root);

    /* ---- 标题 + 进度 ---- */
    lv_obj_t *title = ui_label(s_root, "开机自检", &ui_cjk_24, C_TXT);
    lv_obj_set_pos(title, 24, 24);

    s_prog = ui_label(s_root, "0/5", &ui_cjk_16, 0x8FA8CC);
    lv_obj_align(s_prog, LV_ALIGN_TOP_RIGHT, -24, 30);

    /* ---- 5 行检查项 ---- */
    for (int i = 0; i < BOOT_ITEM_CNT; i++) {
        int y = 78 + i * 44;

        lv_obj_t *dot = lv_obj_create(s_root);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(dot, 9, 9);
        lv_obj_set_pos(dot, 26, y + 7);
        lv_obj_set_style_radius(dot, 5, 0);
        lv_obj_set_style_bg_color(dot, col(0x39496B), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        s_dots[i] = dot;

        lv_obj_t *nm = ui_label(s_root, s_names[i], &ui_cjk_16, C_TXT);
        lv_obj_set_pos(nm, 46, y);

        lv_obj_t *val = ui_label(s_root, "检测中", &ui_cjk_16, 0x5B6B85);
        lv_obj_set_width(val, 200);
        lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(val, 236, y);
        s_vals[i] = val;
    }

    /* ---- 底部状态条 + 文案 + 版本 ---- */
    s_bar = lv_obj_create(s_root);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_bar, 432, 3);
    lv_obj_set_pos(s_bar, 24, 318);
    lv_obj_set_style_radius(s_bar, 2, 0);
    lv_obj_set_style_bg_color(s_bar, col(0x22D3EE), 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);

    s_hint = ui_label(s_root, "正在自检…", &ui_cjk_16, 0x8FA8CC);
    lv_obj_set_pos(s_hint, 24, 332);

    /* 这一行原先是 montserrat_14，只能渲染 ASCII，中文会显示成方框 */
    lv_obj_t *ver = ui_label(s_root, "ESP32-P4C5", &lv_font_montserrat_14, 0x4E5D78);
    lv_obj_set_pos(ver, 24, 362);

    lv_obj_t *skip = lv_button_create(s_root);
    lv_obj_set_size(skip, 68, 32);
    lv_obj_align(skip, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_set_style_bg_color(skip, col(0x1B2440), 0);
    lv_obj_set_style_bg_opa(skip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(skip, 1, 0);
    lv_obj_set_style_border_color(skip, col(0x2A3550), 0);
    lv_obj_set_style_radius(skip, 8, 0);
    lv_obj_set_style_pad_all(skip, 0, 0);
    lv_obj_add_event_cb(skip, boot_skip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = ui_label(skip, "跳过", &ui_cjk_16, 0x8FA8CC);
    lv_obj_center(sl);

    s_timer = lv_timer_create(boot_tick_cb, 100, NULL);

    ESP_LOGI(TAG, "boot check UI ready (%d items)", BOOT_ITEM_CNT);
}

void ui_boot_check_mark(int item, bool ok, const char *text)
{
    if (item < 0 || item >= BOOT_ITEM_CNT) {
        return;
    }
    if (text && text[0]) {
        strlcpy(s_text[item], text, sizeof(s_text[item]));
    }
    s_state[item] = ok ? 1 : 0;
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
    /* 还没上报的按"跳过"处理，避免一直转在检测中 */
    for (int i = 0; i < BOOT_ITEM_CNT; i++) {
        if (s_state[i] == -1) {
            s_state[i] = -2;
        }
    }
    s_done = true;
}
