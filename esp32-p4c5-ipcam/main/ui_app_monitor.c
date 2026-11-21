/*
 * 监控 App —— 实时画面 + 推流状态
 */
#include "lvgl.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "app_snapshot.h"
#include "rtsp_server.h"
#include "app_encoder.h"
#include "wifi_mgr.h"

#define AREA_Y      0
#define AREA_H      480

static lv_obj_t *s_lbl_rtsp;

/* 每次重建界面前清空控件指针。
 * App 没开"后台"时退出会销毁整个界面，静态指针就此悬空；
 * 下一次 create 里若还有函数先用到它（比如按旧按钮改样式），
 * 就会 Load access fault 直接重启。 */
static void reset_ui_refs(void)
{
    s_lbl_rtsp = NULL;
}

static void create(lv_obj_t *parent)
{
    reset_ui_refs();
    /* 全屏画面：不再留顶栏，状态文字直接叠在画面上 */
    lv_obj_t *img = ui_shell_preview_attach(parent);
    lv_obj_set_size(img, 480, AREA_H);
    lv_obj_set_pos(img, 0, AREA_Y);

    /* 左上：推流状态 */
    s_lbl_rtsp = lv_label_create(parent);
    lv_label_set_text(s_lbl_rtsp, "RTSP: --");
    lv_obj_set_style_text_font(s_lbl_rtsp, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_lbl_rtsp, col(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_lbl_rtsp, col(0x000000), 0);
    lv_obj_set_style_bg_opa(s_lbl_rtsp, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_lbl_rtsp, 5, 0);
    lv_obj_set_style_radius(s_lbl_rtsp, 8, 0);
    lv_obj_set_pos(s_lbl_rtsp, 8, 8);
}

static void tick(void)
{
    /* 推流状态 */
    uint32_t pkts, bytes;
    int clients;
    rtsp_server_get_stats(&pkts, &bytes, &clients);
    uint32_t frames, keyframes, last_size;
    app_encoder_get_stats(&frames, &keyframes, &last_size);
    if (clients > 0) {
        lv_label_set_text_fmt(s_lbl_rtsp, "RTSP %d client(s) %lu fps", clients,
                              (unsigned long)frames);
    } else {
        const char *ip = wifi_mgr_get_ip();
        lv_label_set_text_fmt(s_lbl_rtsp, "rtsp://%s:554/stream", ip);
    }
}

static snap_ar_t s_saved_ar;
static bool s_ar_saved;

/* 监控固定 1:1 画面，不受相机 App 的画幅设置影响 */
static void on_show(void)
{
    ui_shell_set_tick_cb(tick);
    if (!s_ar_saved) {
        s_saved_ar = app_snapshot_get_aspect();
        s_ar_saved = true;
    }
    if (app_snapshot_get_aspect() != SNAP_AR_1_1) {
        app_snapshot_set_aspect(SNAP_AR_1_1);
        ui_shell_preview_clear();
    }
}

static void on_hide(void)
{
    ui_shell_set_tick_cb(NULL);
    if (s_ar_saved) {
        app_snapshot_set_aspect(s_saved_ar);
        s_ar_saved = false;
        ui_shell_preview_clear();
    }
}

const ui_app_t ui_app_monitor = {
    .name = "监控",
    .symbol = LV_SYMBOL_VIDEO,
    .color = 0x185FA5,
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
