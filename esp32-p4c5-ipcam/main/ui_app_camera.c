/*
 * 相机 App —— 全屏预览 + 快门拍照（JPEG 存 flash，TF 卡后续切换）
 */
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "app_jpeg.h"
#include "app_snapshot.h"
#include "app_storage.h"

static const char *TAG = "ui_camera";

#define AREA_Y      0
#define AREA_H      480

#define SNAP_TIMEOUT_MS 3000

static lv_obj_t *s_lbl_count;
static lv_obj_t *s_lbl_hint;
static lv_obj_t *s_btn_shutter;
static lv_obj_t *s_ar_btns[3];
static lv_obj_t *s_ar_lbls[3];
static lv_obj_t *s_btn_rotate;
static lv_obj_t *s_lbl_orient;

/* 拍照请求与状态（LVGL 与 worker 之间） */
static volatile bool s_capture_req;
static volatile int  s_capture_state;   /* 0 idle, 1 busy, 2 ok, 3 fail */
static char s_capture_name[32];
static char s_err[48];
static int  s_photo_count; 

static SemaphoreHandle_t s_cam_sem;

static void worker_task(void *arg)
{
    (void)arg;
    while (1) {
        /* 事件驱动：没请求就永久阻塞，不占 CPU */
        if (xSemaphoreTake(s_cam_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_capture_req = false;
        s_capture_state = 1;

        /* 请求一张全分辨率快照（下一个相机帧到达时生成） */
        app_snapshot_request();
        int waited = 0;
        while (!app_snapshot_ready() && waited < SNAP_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
        }
        if (!app_snapshot_ready()) {
            ESP_LOGW(TAG, "snapshot timeout (camera pipeline idle?)");
            strlcpy(s_err, "相机无画面（等 3 秒超时）", sizeof(s_err));
            s_capture_state = 3;
            continue;
        }

        /* 按当前画幅做 PPA 转换（在任务里做，不受帧率影响） */
        int brc = app_snapshot_build();
        if (brc != 0) {
            ESP_LOGE(TAG, "snapshot build failed (%d)", brc);
            strlcpy(s_err, "图像转换失败", sizeof(s_err));
            s_capture_state = 3;
            app_snapshot_clear();
            continue;
        }

        int w = 0, h = 0;
        const uint8_t *snap = app_snapshot_get(&w, &h);
        if (!snap || w <= 0 || h <= 0) {
            ESP_LOGE(TAG, "snapshot null %dx%d", w, h);
            strlcpy(s_err, "快照数据无效", sizeof(s_err));
            s_capture_state = 3;
            app_snapshot_clear();
            continue;
        }

        /* 编码器按快照尺寸初始化（首次会重建为全分辨率） */
        esp_err_t jerr = app_jpeg_init(w, h);
        if (jerr != ESP_OK) {
            ESP_LOGE(TAG, "jpeg init %dx%d failed", w, h);
            strlcpy(s_err, "JPEG 编码器初始化失败", sizeof(s_err));
            s_capture_state = 3;
            app_snapshot_clear();
            continue;
        }

        const uint8_t *jpg = NULL;
        int len = app_jpeg_encode_rgb565(snap, (size_t)w * h * 2, &jpg);
        app_snapshot_clear();

        if (len <= 0 || !jpg) {
            ESP_LOGE(TAG, "jpeg encode failed (%d)", len);
            strlcpy(s_err, "JPEG 编码失败", sizeof(s_err));
            s_capture_state = 3;
            continue;
        }
        /* 校验 JPEG 头尾，能提前发现数据错乱 */
        if (!(jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[len - 2] == 0xFF && jpg[len - 1] == 0xD9)) {
            ESP_LOGW(TAG, "jpeg markers bad: %02x%02x ... %02x%02x",
                     jpg[0], jpg[1], jpg[len - 2], jpg[len - 1]);
        }
        ESP_LOGI(TAG, "photo %dx%d -> %d KB", w, h, len / 1024);

        if (app_storage_save_photo(jpg, (size_t)len, s_capture_name, sizeof(s_capture_name))) {
            s_photo_count++;
            s_capture_state = 2;
        } else {
            int used = 0, free_kb = 0;
            app_storage_usage(&used, &free_kb);
            ESP_LOGE(TAG, "save failed, free %d KB", free_kb);
            snprintf(s_err, sizeof(s_err), "存储写入失败（剩余 %d KB）", free_kb);
            s_capture_state = 3;
        }
    }
}

static void shutter_cb(lv_event_t *e)
{
    (void)e;
    if (s_capture_state == 1) {
        return;
    }
    s_capture_req = true;
    if (s_cam_sem) {
        xSemaphoreGive(s_cam_sem);
    }
    lv_obj_set_style_bg_color(s_btn_shutter, col(0x888888), 0);
}

static void orient_btn_cb(lv_event_t *e);

static void ar_btn_refresh(void)
{
    snap_ar_t cur = app_snapshot_get_aspect();
    bool p = app_snapshot_is_portrait();
    static const char *land[3] = { "1:1", "4:3", "16:9" };
    static const char *port[3] = { "1:1", "3:4", "9:16" };
    for (int i = 0; i < 3; i++) {
        if (!s_ar_btns[i]) {
            continue;
        }
        bool on = (i == (int)cur);
        lv_obj_set_style_bg_color(s_ar_btns[i], col(on ? C_ACCENT : 0x1D3C74), 0);
        lv_obj_set_style_bg_opa(s_ar_btns[i], on ? LV_OPA_COVER : (lv_opa_t)150, 0);
        if (s_ar_lbls[i]) {
            lv_label_set_text(s_ar_lbls[i], p ? port[i] : land[i]);
        }
    }
    if (s_btn_rotate) {
        lv_obj_set_style_bg_color(s_btn_rotate, col(p ? 0xB07A2E : 0x1D3C74), 0);
    }
}

static void ar_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 2) {
        return;
    }
    app_snapshot_set_aspect((snap_ar_t)idx);
    ar_btn_refresh();
    ui_shell_preview_clear();      /* 清掉旧画幅的黑边/旧画面 */
    ui_shell_toast(app_snapshot_aspect_name((snap_ar_t)idx));
}

/* 竖屏/横屏切换 */
static void rotate_btn_cb(lv_event_t *e)
{
    (void)e;
    bool p = !app_snapshot_is_portrait();
    app_snapshot_set_portrait(p);
    ar_btn_refresh();
    ui_shell_preview_clear();
    ui_shell_toast(p ? "竖屏拍照" : "横屏拍照");
}

/* 画面方向补偿：结构件里摄像头是翻转安装的，点一下转 90°，存 NVS */
static void orient_btn_cb(lv_event_t *e)
{
    (void)e;
    int deg = (app_snapshot_rot_off() + 90) % 360;
    app_snapshot_set_rot_off(deg);
    if (s_lbl_orient) {
        lv_label_set_text_fmt(s_lbl_orient, "%d", deg);
    }
    ui_shell_preview_clear();
    ui_shell_toast("画面方向已调整");
}

/* 每次重建界面前清空控件指针。
 * App 没开"后台"时退出会销毁整个界面，静态指针就此悬空；
 * 下一次 create 里若还有函数先用到它（比如按旧按钮改样式），
 * 就会 Load access fault 直接重启。 */
static void reset_ui_refs(void)
{
    s_lbl_count = NULL;
    s_lbl_hint = NULL;
    s_btn_shutter = NULL;
    memset(s_ar_btns, 0, sizeof(s_ar_btns));
    memset(s_ar_lbls, 0, sizeof(s_ar_lbls));
    s_btn_rotate = NULL;
    s_lbl_orient = NULL;
}

static void create(lv_obj_t *parent)
{
    reset_ui_refs();
    /* 全屏取景：去掉顶栏，提示文字叠在画面上 */
    lv_obj_t *img = ui_shell_preview_attach(parent);
    lv_obj_set_size(img, 480, AREA_H);
    lv_obj_set_pos(img, 0, AREA_Y);

    /* 快门 */
    s_btn_shutter = lv_button_create(parent);
    lv_obj_set_size(s_btn_shutter, 72, 72);
    lv_obj_set_pos(s_btn_shutter, 240 - 36, 480 - 100);
    lv_obj_set_style_radius(s_btn_shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_btn_shutter, col(0xFFFFFF), 0);
    lv_obj_set_style_border_color(s_btn_shutter, col(0x185FA5), 0);
    lv_obj_set_style_border_width(s_btn_shutter, 6, 0);
    lv_obj_add_event_cb(s_btn_shutter, shutter_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = lv_label_create(s_btn_shutter);
    lv_label_set_text(sl, "");
    lv_obj_center(sl);

    s_lbl_hint = ui_label(parent, "", NULL, C_TXT);
    lv_obj_set_style_bg_color(s_lbl_hint, col(0x000000), 0);
    lv_obj_set_style_bg_opa(s_lbl_hint, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_lbl_hint, 6, 0);
    lv_obj_set_style_radius(s_lbl_hint, 8, 0);
    lv_obj_align(s_lbl_hint, LV_ALIGN_TOP_MID, 0, AREA_Y + 8);

    s_lbl_count = ui_label(parent, "照片：0 张", NULL, C_TXT);
    lv_obj_set_style_bg_color(s_lbl_count, col(0x000000), 0);
    lv_obj_set_style_bg_opa(s_lbl_count, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(s_lbl_count, 6, 0);
    lv_obj_set_style_radius(s_lbl_count, 8, 0);
    lv_obj_align(s_lbl_count, LV_ALIGN_TOP_LEFT, 8, AREA_Y + 8);

    /* 画幅切换：1:1 / 4:3 / 16:9（默认 1:1，与方屏匹配） */
    static const char *ar_txt[3] = { "1:1", "4:3", "16:9" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(parent);
        lv_obj_set_size(b, 62, 34);
        lv_obj_set_pos(b, 10 + i * 68, 480 - 152);
        lv_obj_set_style_radius(b, 17, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, ar_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, ar_txt[i], NULL, C_TXT);
        lv_obj_center(l);
        s_ar_btns[i] = b;
        s_ar_lbls[i] = l;
    }

    /* 旋转：横屏 / 竖屏拍照切换 */
    s_btn_rotate = lv_button_create(parent);
    lv_obj_set_size(s_btn_rotate, 62, 34);
    lv_obj_set_pos(s_btn_rotate, 10 + 3 * 68, 480 - 152);
    lv_obj_set_style_radius(s_btn_rotate, 17, 0);
    lv_obj_set_style_pad_all(s_btn_rotate, 0, 0);
    lv_obj_add_event_cb(s_btn_rotate, rotate_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = ui_label(s_btn_rotate, "旋转", NULL, C_TXT);
    lv_obj_center(rl);

    /* 画面方向补偿（0/90/180/270）：结构件里摄像头是翻转安装的 */
    lv_obj_t *ob = lv_button_create(parent);
    lv_obj_set_size(ob, 62, 34);
    lv_obj_set_pos(ob, 10 + 4 * 68, 480 - 152);
    lv_obj_set_style_radius(ob, 17, 0);
    lv_obj_set_style_pad_all(ob, 0, 0);
    lv_obj_set_style_bg_color(ob, col(0x1D3C74), 0);
    lv_obj_add_event_cb(ob, orient_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_orient = ui_label(ob, "180", &lv_font_montserrat_14, C_TXT);
    lv_obj_center(s_lbl_orient);
    lv_label_set_text_fmt(s_lbl_orient, "%d", app_snapshot_rot_off());

    ar_btn_refresh();

    /* 初始化存储（编码器在拍照时按快照尺寸初始化） */
    if (app_storage_mount()) {
        char all[32][32];
        s_photo_count = app_storage_list_photos(all, 32);
    }

    static bool worker_started;
    if (!worker_started) {
        worker_started = true;
        s_cam_sem = xSemaphoreCreateBinary();
        /* 必须用内部栈：无 TF 卡时会写 flash FATFS，
         * 而 flash 写会断言"当前栈必须在内部 DRAM"（PSRAM 栈必崩）。R-05 */
        if (xTaskCreate(worker_task, "cam_worker", 8192, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "create cam_worker failed");
        }
    }
}

static void tick(void)
{
    lv_label_set_text_fmt(s_lbl_count, "照片：%d 张", s_photo_count);

    if (s_capture_state == 1) {
        lv_label_set_text(s_lbl_hint, "正在保存…");
    } else if (s_capture_state == 2) {
        lv_label_set_text_fmt(s_lbl_hint, "已保存 %s", s_capture_name);
        s_capture_state = 0;
        lv_obj_set_style_bg_color(s_btn_shutter, col(0xFFFFFF), 0);
    } else if (s_capture_state == 3) {
        lv_label_set_text_fmt(s_lbl_hint, "%s", s_err[0] ? s_err : "保存失败");
        s_capture_state = 0;
        lv_obj_set_style_bg_color(s_btn_shutter, col(0xFFFFFF), 0);
    } else {
        lv_label_set_text(s_lbl_hint, "");
    }
}

static void on_show(void) { ui_shell_set_tick_cb(tick); }
static void on_hide(void) { ui_shell_set_tick_cb(NULL); }

const ui_app_t ui_app_camera = {
    .name = "相机",
    .symbol = LV_SYMBOL_EYE_OPEN,
    .color = 0x2E7D5B,
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
