/*
 * 相册 App —— 照片列表 / 图标两种视图（软件 JPEG 解码）/ 查看 / 删除
 *
 * 列表：一行一张，右侧 × 直接删（不用进大图）
 * 图标：一屏 4 张缩略图（2×2），底部翻页
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "app_storage.h"
#include "jpeg_decoder.h"
static const char *TAG = "ui_gallery";

#define MAX_PHOTOS  32
#define GRID_COLS   2
#define GRID_ROWS   2
#define GRID_PER    (GRID_COLS * GRID_ROWS)

static char s_names[MAX_PHOTOS][32];
static int  s_count;

static lv_obj_t *s_root;        /* 满屏根容器（避免 app_shift_up 的 34px 偏移） */
static lv_obj_t *s_list;        /* 列表容器 */
static lv_obj_t *s_grid;        /* 图标容器 */
static lv_obj_t *s_pager;       /* 翻页条（仅图标视图） */
static lv_obj_t *s_pager_lbl;
static lv_obj_t *s_mode_btns[2];
static lv_obj_t *s_lbl_empty;
static lv_obj_t *s_viewer;
static lv_obj_t *s_viewer_img;
static int  s_view_idx = -1;
static char s_view_name[32];    /* 看图时锁定的文件名（防止下标错位删错） */

static int  s_mode;             /* 0 = 列表，1 = 图标 */
static int  s_page;

static uint8_t *s_decode_buf;
static lv_image_dsc_t s_view_dsc;

/* 缩略图：1/4 解码，四张一页 */
#define TH_MAX_W    480
#define TH_MAX_H    272
static uint8_t *s_thumb[GRID_PER];
static lv_image_dsc_t s_thumb_dsc[GRID_PER];

/* 查看器缩放/平移 */
#define VIEW_W      480
#define VIEW_H      480
static int s_zoom = 256;      /* 256 = 1.0x（适配屏幕） */
static int s_fit_zoom = 256;  /* 适配屏幕的基准倍率 */
static int s_img_w, s_img_h;
static int s_off_x, s_off_y;

static void refresh_list(void);

/* ================= 查看器 ================= */

static void viewer_apply_zoom(void)
{
    if (!s_viewer_img) {
        return;
    }
    lv_image_set_scale(s_viewer_img, (uint32_t)s_zoom);

    int sw = (int)((int64_t)s_img_w * s_zoom / 256);
    int sh = (int)((int64_t)s_img_h * s_zoom / 256);
    int max_x = (sw > VIEW_W) ? (sw - VIEW_W) / 2 : 0;
    int max_y = (sh > VIEW_H) ? (sh - VIEW_H) / 2 : 0;

    if (s_off_x > max_x) s_off_x = max_x;
    if (s_off_x < -max_x) s_off_x = -max_x;
    if (s_off_y > max_y) s_off_y = max_y;
    if (s_off_y < -max_y) s_off_y = -max_y;

    lv_obj_align(s_viewer_img, LV_ALIGN_CENTER, s_off_x, s_off_y);
}

static void viewer_dblclick_cb(lv_event_t *e)
{
    (void)e;
    if (s_view_idx < 0) {
        return;
    }
    s_zoom = (s_zoom > s_fit_zoom) ? s_fit_zoom : (s_fit_zoom * 3);
    s_off_x = 0;
    s_off_y = 0;
    viewer_apply_zoom();
}

static void viewer_pressing_cb(lv_event_t *e)
{
    if (s_view_idx < 0 || s_zoom <= s_fit_zoom) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    s_off_x += v.x;
    s_off_y += v.y;
    viewer_apply_zoom();
}

static void load_photo(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return;
    }
    static uint8_t *file_buf;
    const size_t file_cap = 3 * 1024 * 1024;
    if (!file_buf) {
        file_buf = heap_caps_malloc(file_cap, MALLOC_CAP_SPIRAM);
    }
    if (!file_buf || !s_decode_buf) {
        ui_shell_toast("内存不足");
        return;
    }
    int fsize = app_storage_photo_size(s_names[idx]);
    int len = app_storage_read_photo(s_names[idx], file_buf, file_cap);
    if (len <= 0) {
        ESP_LOGE(TAG, "read %s failed", s_names[idx]);
        ui_shell_toast("读取失败");
        return;
    }
    if (fsize > 0 && len < fsize) {
        ESP_LOGE(TAG, "%s truncated: %d / %d bytes", s_names[idx], len, fsize);
        ui_shell_toast("文件读取不完整");
        return;
    }

    esp_jpeg_image_cfg_t jcfg = {
        .indata = file_buf,
        .indata_size = (uint32_t)len,
        .outbuf = s_decode_buf,
        .outbuf_size = 1920 * 1080 * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags.swap_color_bytes = 0,
    };
    esp_jpeg_image_output_t jout = { 0 };
    esp_err_t err = esp_jpeg_decode(&jcfg, &jout);
    if (err != ESP_OK || jout.width == 0 || jout.width > 1920 || jout.height > 1080) {
        ESP_LOGE(TAG, "decode %s failed: %d (%dx%d)", s_names[idx], err, jout.width, jout.height);
        ui_shell_toast("解码失败");
        return;
    }
    ESP_LOGI(TAG, "show %s: %d KB, %dx%d", s_names[idx], fsize / 1024, jout.width, jout.height);

    memset(&s_view_dsc, 0, sizeof(s_view_dsc));
    s_view_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_view_dsc.header.w = jout.width;
    s_view_dsc.header.h = jout.height;
    s_view_dsc.header.stride = jout.width * 2;
    s_view_dsc.data = s_decode_buf;
    s_view_dsc.data_size = jout.width * jout.height * 2;
    lv_image_set_src(s_viewer_img, &s_view_dsc);

    int fit = (jout.width > jout.height) ? (VIEW_W * 256 / jout.width)
                                         : (VIEW_H * 256 / jout.height);
    s_fit_zoom = fit;
    s_zoom = fit;
    s_img_w = jout.width;
    s_img_h = jout.height;
    s_off_x = 0;
    s_off_y = 0;
    lv_image_set_antialias(s_viewer_img, true);
    viewer_apply_zoom();

    s_view_idx = idx;
    strlcpy(s_view_name, s_names[idx], sizeof(s_view_name));
    lv_obj_remove_flag(s_viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_viewer);
    ui_shell_set_gesture_enabled(false);
}

static void viewer_close(void)
{
    if (s_viewer) {
        lv_obj_add_flag(s_viewer, LV_OBJ_FLAG_HIDDEN);
    }
    s_view_idx = -1;
    s_zoom = 256;
    s_off_x = s_off_y = 0;
    ui_shell_set_gesture_enabled(true);
}

/* 按文件名删：统一入口，失败要能看见原因 */
static void del_by_name(const char *name)
{
    if (!name || !name[0]) {
        return;
    }
    if (!app_storage_delete_photo(name)) {
        ESP_LOGE(TAG, "delete %s failed (errno=%d)", name, errno);
        ui_shell_toast("删除失败");
        return;
    }
    ESP_LOGI(TAG, "deleted %s", name);
    ui_shell_toast("已删除");
    refresh_list();
}

static void viewer_btn_cb(lv_event_t *e)
{
    const char *cmd = (const char *)lv_event_get_user_data(e);
    if (strcmp(cmd, "close") == 0) {
        viewer_close();
    } else if (strcmp(cmd, "prev") == 0) {
        if (s_view_idx > 0) load_photo(s_view_idx - 1);
    } else if (strcmp(cmd, "next") == 0) {
        if (s_view_idx < s_count - 1) load_photo(s_view_idx + 1);
    } else if (strcmp(cmd, "del") == 0 && s_view_idx >= 0) {
        char nm[32];
        strlcpy(nm, s_view_name, sizeof(nm));
        viewer_close();
        del_by_name(nm);
    }
}

/* ================= 列表 / 图标 两种视图 ================= */

static void row_click_cb(lv_event_t *e)
{
    load_photo((int)(intptr_t)lv_event_get_user_data(e));
}

static void del_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_count) {
        return;
    }
    char nm[32];
    strlcpy(nm, s_names[idx], sizeof(nm));
    del_by_name(nm);
}

static void build_list_view(void)
{
    lv_obj_clean(s_list);
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *row = lv_button_create(s_list);
        lv_obj_set_size(row, 424, 44);
        lv_obj_set_style_bg_color(row, col(0x18356B), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, s_names[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_t *del = lv_button_create(row);
        lv_obj_set_size(del, 32, 30);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 2, 0);
        lv_obj_set_style_bg_color(del, col(0x6B2038), 0);
        lv_obj_set_style_radius(del, 8, 0);
        lv_obj_set_style_pad_all(del, 0, 0);
        lv_obj_add_event_cb(del, del_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *dl = ui_label(del, "删", &ui_cjk_16, 0xFFB0B0);
        lv_obj_center(dl);
    }
}

static bool thumb_load(int slot, int idx)
{
    static uint8_t *file_buf;
    const size_t file_cap = 3 * 1024 * 1024;
    if (!file_buf) {
        file_buf = heap_caps_malloc(file_cap, MALLOC_CAP_SPIRAM);
    }
    if (!file_buf || !s_thumb[slot]) {
        return false;
    }
    int len = app_storage_read_photo(s_names[idx], file_buf, file_cap);
    if (len <= 0) {
        return false;
    }
    esp_jpeg_image_cfg_t jcfg = {
        .indata = file_buf,
        .indata_size = (uint32_t)len,
        .outbuf = s_thumb[slot],
        .outbuf_size = TH_MAX_W * TH_MAX_H * 2,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_1_4,     /* 缩略图：1/4 解码，快很多 */
        .flags.swap_color_bytes = 0,
    };
    esp_jpeg_image_output_t jout = { 0 };
    if (esp_jpeg_decode(&jcfg, &jout) != ESP_OK || jout.width == 0) {
        return false;
    }
    memset(&s_thumb_dsc[slot], 0, sizeof(s_thumb_dsc[slot]));
    s_thumb_dsc[slot].header.cf = LV_COLOR_FORMAT_RGB565;
    s_thumb_dsc[slot].header.w = jout.width;
    s_thumb_dsc[slot].header.h = jout.height;
    s_thumb_dsc[slot].header.stride = jout.width * 2;
    s_thumb_dsc[slot].data = s_thumb[slot];
    s_thumb_dsc[slot].data_size = jout.width * jout.height * 2;
    return true;
}

/* 缩略图下方的说明：优先显示拍摄日期（对齐设计稿 06-gallery 的"11-21 08:14"），
 * 拿不到 mtime 时退回文件名（去掉扩展名）。 */
static void photo_caption(const char *name, char *out, size_t n)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", app_storage_root(), name);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_mtime > 1735689600) {
        struct tm tm;
        time_t t = st.st_mtime;
        localtime_r(&t, &tm);
        snprintf(out, n, "%02d-%02d %02d:%02d",
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
        return;
    }
    size_t len = strcspn(name, ".");
    snprintf(out, n, "%.*s", (int)len, name);
}

static void build_grid_view(void)
{
    lv_obj_clean(s_grid);
    int pages = (s_count + GRID_PER - 1) / GRID_PER;
    if (pages < 1) {
        pages = 1;
    }
    if (s_page >= pages) {
        s_page = pages - 1;
    }
    int base = s_page * GRID_PER;

    for (int k = 0; k < GRID_PER; k++) {
        int idx = base + k;
        lv_obj_t *cell = lv_obj_create(s_grid);
        lv_obj_set_size(cell, 212, 186);
        lv_obj_set_pos(cell, (k % GRID_COLS) * 220, (k / GRID_COLS) * 192);
        lv_obj_set_style_bg_color(cell, col(0x10254A), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, col(0x24406F), 0);
        lv_obj_set_style_radius(cell, 10, 0);
        lv_obj_set_style_pad_all(cell, 4, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

        if (idx >= s_count) {
            lv_obj_t *l = ui_label(cell, "空", NULL, 0x2E3F5C);
            lv_obj_center(l);
            continue;
        }

        bool ok = thumb_load(k, idx);
        lv_obj_t *img = lv_image_create(cell);
        lv_obj_set_size(img, 200, 150);
        if (ok) {
            lv_image_set_src(img, &s_thumb_dsc[k]);
            /* 铺满 200x150（cover）：取较大的倍率，多出来的部分被控件裁掉，
             * 这样四格看起来一样大，不会出现"留黑边/填不满" */
            int w = s_thumb_dsc[k].header.w;
            int h = s_thumb_dsc[k].header.h;
            int sc_w = 200 * 256 / w;
            int sc_h = 150 * 256 / h;
            int sc = (sc_w > sc_h) ? sc_w : sc_h;
            if (sc < 1) {
                sc = 1;
            }
            if (sc > 1024) {
                sc = 1024;
            }
            lv_image_set_scale(img, (uint32_t)sc);
        } else {
            lv_obj_set_style_bg_color(img, col(0x0A1630), 0);
            lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
        }
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 0);

        char cap[24];
        photo_caption(s_names[idx], cap, sizeof(cap));
        lv_obj_t *nm = ui_label(cell, cap, NULL, 0xC8D6EA);
        lv_obj_align(nm, LV_ALIGN_BOTTOM_LEFT, 2, 0);

        /* 题目区（图片+名字）点击看大图 */
        lv_obj_add_flag(img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(img, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
        lv_obj_add_event_cb(nm, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
        lv_obj_add_flag(nm, LV_OBJ_FLAG_CLICKABLE);

        /* 右上角删除（直接删，不必进大图） */
        lv_obj_t *del = lv_button_create(cell);
        lv_obj_set_size(del, 34, 30);
        lv_obj_align(del, LV_ALIGN_TOP_RIGHT, 2, -2);
        lv_obj_set_style_bg_color(del, col(0x6B2038), 0);
        lv_obj_set_style_radius(del, 8, 0);
        lv_obj_set_style_pad_all(del, 0, 0);
        lv_obj_add_event_cb(del, del_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
        lv_obj_t *dl = ui_label(del, "删", &ui_cjk_16, 0xFFB0B0);
        lv_obj_center(dl);
    }

    if (s_pager_lbl) {
        lv_label_set_text_fmt(s_pager_lbl, "%d / %d", s_page + 1, pages);
    }
}

static void mode_refresh(void)
{
    if (s_mode == 0) {
        lv_obj_add_flag(s_grid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pager, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        build_list_view();
    } else {
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_pager, LV_OBJ_FLAG_HIDDEN);
        build_grid_view();
    }
    for (int i = 0; i < 2; i++) {
        if (!s_mode_btns[i]) {
            continue;
        }
        bool on = (s_mode == i);
        lv_obj_set_style_bg_color(s_mode_btns[i], col(on ? C_ACCENT : 0x16203A), 0);
    }
}

static void refresh_list(void)
{
    s_count = app_storage_list_photos(s_names, MAX_PHOTOS);
    if (s_count == 0) {
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_grid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pager, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    mode_refresh();
}

static void mode_btn_cb(lv_event_t *e)
{
    s_mode = (int)(intptr_t)lv_event_get_user_data(e);
    s_page = 0;
    mode_refresh();
}

static void page_btn_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int pages = (s_count + GRID_PER - 1) / GRID_PER;
    int p = s_page + d;
    if (p < 0) {
        p = 0;
    }
    if (p >= pages) {
        p = pages - 1;
    }
    if (p == s_page) {
        return;
    }
    s_page = p;
    build_grid_view();
}

/* 每次重建界面前清空控件指针（退出销毁后这些指针会悬空） */
static void reset_ui_refs(void)
{
    s_root = NULL;
    s_list = NULL;
    s_grid = NULL;
    s_pager = NULL;
    s_pager_lbl = NULL;
    s_lbl_empty = NULL;
    s_viewer = NULL;
    s_viewer_img = NULL;
    s_mode_btns[0] = s_mode_btns[1] = NULL;
}

static void create(lv_obj_t *parent)
{
    reset_ui_refs();
    ui_app_topbar(parent, "相册");

    /* 所有内容放进一个 y=0 的满屏容器：app_shift_up 只会上移 y>=34 的直接子对象，
     * 放进容器后内部坐标就是屏幕真实坐标，不会再"翻页条被挤出屏幕" */
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, 480, 480);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* 视图切换（左上） */
    static const char *mtxt[2] = { "列表", "图标" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(s_root);
        lv_obj_set_size(b, 76, 30);
        lv_obj_set_pos(b, 12 + i * 84, 2);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, col(i == 0 ? C_ACCENT : 0x16203A), 0);
        lv_obj_add_event_cb(b, mode_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, mtxt[i], &ui_cjk_16, C_TXT);
        lv_obj_center(l);
        s_mode_btns[i] = b;
    }

    /* 列表容器 */
    s_list = lv_obj_create(s_root);
    lv_obj_set_size(s_list, 456, 386);
    lv_obj_set_pos(s_list, 12, 38);
    lv_obj_set_style_bg_color(s_list, col(0x081226), 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_radius(s_list, 10, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    /* 图标容器：2×2 */
    s_grid = lv_obj_create(s_root);
    lv_obj_set_size(s_grid, 452, 382);
    lv_obj_set_pos(s_grid, 14, 38);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);

    /* 翻页条 */
    s_pager = lv_obj_create(s_root);
    lv_obj_set_size(s_pager, 456, 40);
    lv_obj_set_pos(s_pager, 12, 430);
    lv_obj_set_style_bg_opa(s_pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pager, 0, 0);
    lv_obj_set_style_pad_all(s_pager, 0, 0);
    lv_obj_clear_flag(s_pager, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *prev = lv_button_create(s_pager);
    lv_obj_set_size(prev, 92, 34);
    lv_obj_set_pos(prev, 0, 3);
    lv_obj_set_style_radius(prev, 8, 0);
    lv_obj_set_style_bg_color(prev, col(0x1D3C74), 0);
    lv_obj_add_event_cb(prev, page_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(-1));
    lv_obj_t *pl = ui_label(prev, "上一页", &ui_cjk_16, C_TXT);
    lv_obj_center(pl);

    s_pager_lbl = ui_label(s_pager, "1 / 1", NULL, C_TXT_DIM);
    lv_obj_center(s_pager_lbl);

    lv_obj_t *next = lv_button_create(s_pager);
    lv_obj_set_size(next, 92, 34);
    lv_obj_set_pos(next, 364, 3);
    lv_obj_set_style_radius(next, 8, 0);
    lv_obj_set_style_bg_color(next, col(0x1D3C74), 0);
    lv_obj_add_event_cb(next, page_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *nl = ui_label(next, "下一页", &ui_cjk_16, C_TXT);
    lv_obj_center(nl);

    s_lbl_empty = ui_label(s_root, "暂无照片，去相机拍一张吧", NULL, C_TXT_DIM);
    lv_obj_center(s_lbl_empty);

    /* 查看器（全屏覆盖） */
    s_viewer = lv_obj_create(s_root);
    lv_obj_set_size(s_viewer, 480, 480);
    lv_obj_set_pos(s_viewer, 0, 0);
    lv_obj_set_style_bg_color(s_viewer, col(0x000000), 0);
    lv_obj_set_style_bg_opa(s_viewer, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_viewer, 0, 0);
    lv_obj_set_style_radius(s_viewer, 0, 0);
    lv_obj_set_style_pad_all(s_viewer, 0, 0);
    lv_obj_clear_flag(s_viewer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_viewer, LV_OBJ_FLAG_HIDDEN);

    s_viewer_img = lv_image_create(s_viewer);
    lv_obj_center(s_viewer_img);
    lv_obj_add_flag(s_viewer_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_viewer_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_viewer_img, viewer_dblclick_cb, LV_EVENT_DOUBLE_CLICKED, NULL);
    lv_obj_add_event_cb(s_viewer, viewer_pressing_cb, LV_EVENT_PRESSING, NULL);

    static const char *b_prev = "prev", *b_next = "next", *b_del = "del", *b_close = "close";
    struct { const char *cmd; const char *txt; int x; int w; uint32_t bg; } btns[] = {
        { b_close, "关闭", 10,  84, 0x39496B },
        { b_prev,  "上一张", 100, 96, 0x1D3C74 },
        { b_next,  "下一张", 300, 96, 0x1D3C74 },
        { b_del,   "删除", 402, 68, 0x6B2038 },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(s_viewer);
        lv_obj_set_size(b, btns[i].w, 44);
        lv_obj_set_pos(b, btns[i].x, 425);
        lv_obj_set_style_bg_color(b, col(btns[i].bg), 0);
        lv_obj_add_event_cb(b, viewer_btn_cb, LV_EVENT_CLICKED, (void *)btns[i].cmd);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, btns[i].txt);
        lv_obj_set_style_text_font(l, &ui_cjk_16, 0);
        lv_obj_center(l);
    }

    if (!s_decode_buf) {
        s_decode_buf = heap_caps_malloc(1920 * 1080 * 2, MALLOC_CAP_SPIRAM);
    }
    for (int i = 0; i < GRID_PER; i++) {
        if (!s_thumb[i]) {
            s_thumb[i] = heap_caps_malloc(TH_MAX_W * TH_MAX_H * 2, MALLOC_CAP_SPIRAM);
        }
    }
    refresh_list();
}

static void on_show(void)
{
    refresh_list();
    if (!s_viewer || lv_obj_has_flag(s_viewer, LV_OBJ_FLAG_HIDDEN)) {
        ui_shell_set_gesture_enabled(true);
    }
}

static void on_hide(void)
{
    if (s_viewer) {
        lv_obj_add_flag(s_viewer, LV_OBJ_FLAG_HIDDEN);
    }
    s_view_idx = -1;
    ui_shell_set_gesture_enabled(true);
}

const ui_app_t ui_app_gallery = {
    .name = "相册",
    .symbol = LV_SYMBOL_IMAGE,
    .color = 0x6B4F1D,
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};

