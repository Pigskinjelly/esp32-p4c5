/*
 * ESP32-P4C5 「口袋监控系统」—— 主流程
 *
 * 架构（LVGL 拥有屏幕，相机帧经 PPA 写预览缓冲，Shell 分发给各 App）：
 *
 *   SC2336(800x800 YUV420)
 *     ├─ PPA#1: 中心裁 768 → RGB565 480x480 → 预览缓冲 x3 ──┐
 *     ├─ PPA#2: 同 crop → 灰度 96x96 → AI 人形检测(运动启发式) │
 *     └─ /dev/video11 H.264 硬编 → RTP → RTSP 554（常驻推流）
 *                                              LVGL Shell → 监控/相机 App 预览
 *
 * App：监控 / 相机 / 相册 / 股票 / 设置；下拉控制中心连 Wi-Fi。
 *
 * 硬件：OSPTEK ESP32-P4C5-Module V1.5 + YDP395BT008-V1 3.95" 480x480（ST7123 触摸）
 */
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/videodev2.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_video_init.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "driver/i2c_master.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_video.h"
#include "app_lcd.h"
#include "app_audio.h"
#include "app_encoder.h"
#include "rtsp_server.h"
#include "app_touch.h"
#include "wifi_mgr.h"
#include "app_jpeg.h"
#include "app_snapshot.h"
#include "app_storage.h"
#include "ui_shell.h"
#include "trade_day.h"
#include "ui_apps.h"
#include "ui_boot.h"
#include "nvs.h"
#include "nvs_flash.h"

#define ALIGN_UP(num, align)    (((num) + ((align) - 1)) & ~((align) - 1))

#define RTSP_PORT               554
#define ENC_BITRATE_BPS         (3 * 1000 * 1000)
#define ENC_FPS                 30

static const char *TAG = "ipcam";

static esp_lcd_panel_handle_t display_panel;
static ppa_client_handle_t ppa_srm_handle = NULL;
static SemaphoreHandle_t s_ppa_lock = NULL;   /* PPA 客户端只能串行使用 */
static size_t data_cache_line_size = 0;
static i2c_master_bus_handle_t i2c_bus = NULL;

static int cam_w = 800;
static int cam_h = 800;
static bool s_audio_ok;

/* 推流用的 YUV420 缓冲（480x480，和预览同尺寸同裁剪，PPA 一次小转换即可） */
static uint8_t *yuv_buf;
static size_t yuv_buf_bytes;

/* ---- 快照（拍照）----
 * 帧回调只做 memcpy 抓原始 YUV 帧，PPA 转换放到拍照任务里做，
 * 避免大尺寸转换被下一帧相机 DMA 覆盖（之前照片花屏的根因）。 */
static uint8_t *s_snap_yuv;          /* 原始帧副本 YUV420 */
static size_t   s_snap_yuv_bytes;
static volatile bool s_snap_req;     /* 请求抓帧 */
static volatile bool s_snap_yuv_ok;  /* 帧已抓到 */
static uint8_t *s_snap_buf;          /* 转换结果 RGB565 */
static size_t   s_snap_buf_bytes;
static volatile bool s_snap_ready;   /* 转换完成 */
static int s_snap_w, s_snap_h;
static snap_ar_t s_snap_ar = SNAP_AR_1_1;
static bool s_snap_portrait;

void app_snapshot_set_portrait(bool on)
{
    s_snap_portrait = on;
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "snap_port", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool app_snapshot_is_portrait(void)
{
    return s_snap_portrait;
}

void app_snapshot_request(void)
{
    s_snap_ready = false;
    s_snap_yuv_ok = false;
    s_snap_req = true;
}

bool app_snapshot_ready(void)
{
    return s_snap_yuv_ok;
}

const uint8_t *app_snapshot_get(int *w, int *h)
{
    if (w) *w = s_snap_w;
    if (h) *h = s_snap_h;
    return s_snap_ready ? s_snap_buf : NULL;
}

void app_snapshot_clear(void)
{
    s_snap_ready = false;
    s_snap_yuv_ok = false;
}

const char *app_snapshot_aspect_name(snap_ar_t ar)
{
    switch (ar) {
    case SNAP_AR_4_3:  return "4:3";
    case SNAP_AR_16_9: return "16:9";
    default:           return "1:1";
    }
}

void app_snapshot_set_aspect(snap_ar_t ar)
{
    s_snap_ar = ar;
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "snap_ar", (int32_t)ar);
        nvs_commit(h);
        nvs_close(h);
    }
}

snap_ar_t app_snapshot_get_aspect(void)
{
    return s_snap_ar;
}

/* 画面方向补偿：摄像头在结构件里是翻转安装的，默认补 180°，
 * 用户也可以在相机 App 里按「画面」逐档调整（存 NVS） */
static int s_cam_rot_off = 180;

int app_snapshot_rot_off(void)
{
    return s_cam_rot_off;
}

void app_snapshot_set_rot_off(int deg)
{
    deg = ((deg % 360) + 360) % 360;
    s_cam_rot_off = (deg / 90) * 90;
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "cam_rot", s_cam_rot_off);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "camera rotation offset -> %d", s_cam_rot_off);
}

/* 画幅/方向相关的公共工具 */

/* 屏幕方向 → PPA 旋转角（PPA 也是逆时针，和 LVGL 一致） */
static ppa_srm_rotation_angle_t cam_ppa_angle(int deg)
{
    switch (deg) {
    case 90:  return PPA_SRM_ROTATION_ANGLE_90;
    case 180: return PPA_SRM_ROTATION_ANGLE_180;
    case 270: return PPA_SRM_ROTATION_ANGLE_270;
    default:  return PPA_SRM_ROTATION_ANGLE_0;
    }
}

/* 把 PPA 目标块之外的边条刷黑（画幅不是 1:1 时上下/左右留黑边） */
static void preview_clear_bars(uint8_t *buf, int dx, int dy, int dw, int dh)
{
    if (!buf) {
        return;
    }
    if (dy > 0) {
        memset(buf, 0, (size_t)SHELL_PREVIEW_W * dy * 2);
    }
    if (dy + dh < SHELL_PREVIEW_H) {
        memset(buf + (size_t)SHELL_PREVIEW_W * (dy + dh) * 2, 0,
               (size_t)SHELL_PREVIEW_W * (SHELL_PREVIEW_H - dy - dh) * 2);
    }
    if (dx > 0 || dx + dw < SHELL_PREVIEW_W) {
        for (int y = dy; y < dy + dh; y++) {
            uint8_t *row = buf + (size_t)y * SHELL_PREVIEW_W * 2;
            if (dx > 0) {
                memset(row, 0, (size_t)dx * 2);
            }
            if (dx + dw < SHELL_PREVIEW_W) {
                memset(row + (size_t)(dx + dw) * 2, 0,
                       (size_t)(SHELL_PREVIEW_W - dx - dw) * 2);
            }
        }
    }
}

/* 按画幅比例算出源裁剪框（居中）
 * 竖屏时宽高对调（16:9 → 9:16）。
 * 1:1 的边长取 480*16/k（k 为整数），保证预览缩放系数正好是 k/16，
 * 否则 PPA 的 1/16 精度会让预览和照片取景不一致。 */
static void snap_crop(int *cw_out, int *ch_out)
{
    int w = cam_w, h = cam_h;
    int rw = 1, rh = 1;
    switch (s_snap_ar) {
    case SNAP_AR_4_3:  rw = 4; rh = 3; break;
    case SNAP_AR_16_9: rw = 16; rh = 9; break;
    default:           rw = 1; rh = 1; break;
    }
    if (s_snap_portrait) {
        int t = rw; rw = rh; rh = t;
    }

    int cw = w;
    int ch = (int)((int64_t)w * rh / rw);
    if (ch > h) {
        ch = h;
        cw = (int)((int64_t)h * rw / rh);
    }
    cw &= ~1;
    ch &= ~1;

    if (rw == rh) {
        /* 1:1：取最大的、能让 480/c 正好等于 k/16 的 c（720p 下就是 640） */
        int c = (cw < ch) ? cw : ch;
        for (int k = 1; k <= 16; k++) {
            if ((SHELL_PREVIEW_W * 16) % k) {
                continue;
            }
            int cc = (SHELL_PREVIEW_W * 16) / k;
            if (cc <= c) {
                c = cc;
                break;
            }
        }
        cw = c & ~1;
        ch = c & ~1;
    }

    *cw_out = cw;
    *ch_out = ch;
}

int app_snapshot_build(void)
{
    if (!s_snap_yuv_ok || !s_snap_yuv) {
        return -1;
    }
    int cw, ch;
    snap_crop(&cw, &ch);
    size_t need = (size_t)cw * ch * 2;
    if (!s_snap_buf || need > s_snap_buf_bytes) {
        if (s_snap_buf) {
            heap_caps_free(s_snap_buf);
        }
        s_snap_buf = heap_caps_aligned_alloc(data_cache_line_size, need, MALLOC_CAP_SPIRAM);
        if (!s_snap_buf) {
            ESP_LOGE(TAG, "snapshot buf alloc failed (%u KB)", (unsigned)(need / 1024));
            s_snap_buf_bytes = 0;
            return -2;
        }
        s_snap_buf_bytes = need;
    }

    int ox = ((cam_w - cw) / 2) & ~1;
    int oy = ((cam_h - ch) / 2) & ~1;

    /* 照片也跟着屏幕方向转，保证"所见即所得" */
    int rot = (ui_shell_camera_rotation() + s_cam_rot_off) % 360;
    bool swap = (rot == 90 || rot == 270);
    int pw = swap ? ch : cw;
    int ph = swap ? cw : ch;

    ppa_srm_oper_config_t s = { 0 };
    s.in.buffer = s_snap_yuv;
    s.in.pic_w = cam_w;
    s.in.pic_h = cam_h;
    s.in.block_w = cw;
    s.in.block_h = ch;
    s.in.block_offset_x = ox;
    s.in.block_offset_y = oy;
    s.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    s.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    s.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;

    s.out.buffer = s_snap_buf;
    s.out.buffer_size = ALIGN_UP(need, data_cache_line_size);
    s.out.pic_w = pw;
    s.out.pic_h = ph;
    s.out.block_offset_x = 0;
    s.out.block_offset_y = 0;
    s.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    s.rotation_angle = cam_ppa_angle(rot);
    s.scale_x = 1.0f;
    s.scale_y = 1.0f;
    s.mode = PPA_TRANS_MODE_BLOCKING;

    xSemaphoreTake(s_ppa_lock, portMAX_DELAY);
    esp_err_t prc = ppa_do_scale_rotate_mirror(ppa_srm_handle, &s);
    xSemaphoreGive(s_ppa_lock);
    if (prc != ESP_OK) {
        ESP_LOGE(TAG, "snapshot ppa failed: %s", esp_err_to_name(prc));
        return -3;
    }
    /* PPA 是 DMA 写 PSRAM，CPU 侧缓存要作废，否则 JPEG 硬编读到脏数据 */
    esp_cache_msync(s_snap_buf, ALIGN_UP(need, data_cache_line_size),
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);

    s_snap_w = pw;
    s_snap_h = ph;
    s_snap_ready = true;
    ESP_LOGI(TAG, "snapshot ready %dx%d (%s, rot %d)", pw, ph,
             app_snapshot_aspect_name(s_snap_ar), rot);
    return 0;
}

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index,
                                         uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                         size_t camera_buf_len, void *user_data);

/*
 * 开机自检：拍一张照片走完整链路（快照 → PPA → JPEG → 写 flash），
 * 串口打印尺寸/大小/JPEG 头尾，然后删掉文件。用于无触摸情况下验证拍照。
 * 设 0 可关闭。
 */
#define SNAP_SELFTEST   0
#define MAP_SELFTEST    0
#define SNAP_SELFTEST   0
#define MAP_SELFTEST    0

#if SNAP_SELFTEST
static void snapshot_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(12000));

    /* 等后台存储挂载完成（最多再等 8 秒） */
    for (int i = 0; i < 80 && !app_storage_ready(); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!app_storage_ready()) {
        ESP_LOGE(TAG, "[selftest] storage not ready, skip write test");
    }
    app_snapshot_request();
    int waited = 0;
    while (!app_snapshot_ready() && waited < 3000) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    if (!app_snapshot_ready()) {
        ESP_LOGE(TAG, "[selftest] snapshot timeout");
        vTaskDelete(NULL);
        return;
    }
    if (app_snapshot_build() != 0) {
        ESP_LOGE(TAG, "[selftest] snapshot build failed");
        app_snapshot_clear();
        vTaskDelete(NULL);
        return;
    }
    int w = 0, h = 0;
    const uint8_t *snap = app_snapshot_get(&w, &h);
    if (!snap) {
        ESP_LOGE(TAG, "[selftest] snapshot null");
        vTaskDelete(NULL);
        return;
    }
    if (app_jpeg_init(w, h) != ESP_OK) {
        ESP_LOGE(TAG, "[selftest] jpeg init failed");
        app_snapshot_clear();
        vTaskDelete(NULL);
        return;
    }
    const uint8_t *jpg = NULL;
    int len = app_jpeg_encode_rgb565(snap, (size_t)w * h * 2, &jpg);
    app_snapshot_clear();
    if (len <= 0 || !jpg) {
        ESP_LOGE(TAG, "[selftest] encode failed (%d)", len);
        vTaskDelete(NULL);
        return;
    }
    bool ok_marker = (jpg[0] == 0xFF && jpg[1] == 0xD8 &&
                      jpg[len - 2] == 0xFF && jpg[len - 1] == 0xD9);
    ESP_LOGI(TAG, "[selftest] jpeg %dx%d, %d KB, SOI/EOI=%s", w, h, len / 1024,
             ok_marker ? "ok" : "BAD");

    char selftest_path[96] = "";
    if (app_storage_ready()) {
        snprintf(selftest_path, sizeof(selftest_path), "%s/_selftest.bin", app_storage_root());
    }
    if (selftest_path[0]) {
        FILE *f = fopen(selftest_path, "wb");
        if (!f) {
            ESP_LOGE(TAG, "[selftest] open %s failed", selftest_path);
            vTaskDelete(NULL);
            return;
        }
        size_t wr = fwrite(jpg, 1, len, f);
        fclose(f);
        int used = 0, free_kb = 0;
        app_storage_usage(&used, &free_kb);
        struct stat st;
        int fsz = (stat(selftest_path, &st) == 0) ? (int)st.st_size : -1;
        ESP_LOGI(TAG, "[selftest] write %u/%d bytes, file=%d, free %d KB",
                 (unsigned)wr, len, fsz, free_kb);
        unlink(selftest_path);
    }
    vTaskDelete(NULL);
}
#endif

/* 编码输出回调：把码流交给 RTSP */
static void encoder_out_cb(const app_enc_frame_t *frame, void *ctx)
{
    rtsp_server_push_frame(frame->data, frame->len, frame->keyframe, frame->pts_us);
}

/* 30 秒心跳：不碰屏幕也能从串口确认系统活着 */
static void heartbeat_task(void *arg)
{
    (void)arg;
    static bool touch_ok;
    touch_ok = (arg != NULL);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        uint32_t frames, keyframes, last_size;
        app_encoder_get_stats(&frames, &keyframes, &last_size);
        uint32_t pkts, bytes;
        int clients;
        rtsp_server_get_stats(&pkts, &bytes, &clients);
        ESP_LOGI(TAG, "[heartbeat] up=%llds touch=%d wifi=%s ip=%s rtsp_clients=%d "
                      "enc=%lu fr heap_int=%lu KB heap_spi=%lu KB",
                 (long long)(esp_timer_get_time() / 1000000), touch_ok,
                 wifi_mgr_state_str(wifi_mgr_get_state()), wifi_mgr_get_ip(),
                 clients, (unsigned long)frames,
                 (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}

static void camera_tune_color(int fd)
{
    (void)fd;
    /* 本板 ISP 的亮度/对比度/饱和度控件由 ISP 设备暴露，sensor 节点(/dev/video0)
     * 会返回 "ctrl id is not supported"，这里不再强设，避免刷错误日志。
     * 颜色偏差改为在 PPA 输出端做 YUV 范围/标准选择处理。 */
}

/* 后台挂载存储：TF 卡优先，失败回落 flash；不阻塞启动 */
static void storage_task(void *arg)
{
    (void)arg;
    if (app_storage_mount()) {
        ESP_LOGI(TAG, "storage ready at %s (%s)", app_storage_root(),
                 app_storage_is_sd() ? "TF card" : "flash");
        ui_boot_check_mark(BOOT_STORE, true, app_storage_is_sd() ? "TF 卡" : "Flash");
    } else {
        ESP_LOGW(TAG, "photo storage mount failed, camera capture disabled");
        ui_boot_check_mark(BOOT_STORE, false, "挂载失败");
    }
    vTaskDelete(NULL);
}

/* 开机自检：把各阶段的结果补齐（内存 / 相机 / 网络），然后收起浮层 */
static void boot_check_task(void *arg)
{
    (void)arg;

    /* 内存：内部 RAM 剩太少会随机重启，这里直接报出来 */
    {
        char t[32];
        size_t in_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
        snprintf(t, sizeof(t), "内部 %uK", (unsigned)in_kb);
        ui_boot_check_mark(BOOT_MEM, in_kb > 30, t);
    }

    /* 相机：最多等 5 秒出第一帧 */
    bool cam = false;
    for (int i = 0; i < 45; i++) {
        if (ui_shell_preview_latest_dsc() != NULL) {
            cam = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ui_boot_check_mark(BOOT_CAM, cam, cam ? "已出图" : "无画面");

    /* 网络：最多等 4 秒（连接慢的不阻塞开机，后面状态栏会更新） */
    bool wifi = false;
    for (int i = 0; i < 20; i++) {
        if (wifi_mgr_get_state() == WIFI_MGR_CONNECTED) {
            wifi = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (wifi) {
        ui_boot_check_mark(BOOT_WIFI, true, wifi_mgr_get_ip());
    } else if (!wifi_mgr_is_on()) {
        ui_boot_check_mark(BOOT_WIFI, false, "已关闭");
    } else {
        /* 连得慢不算故障：开机不等它，状态栏会继续更新 */
        ui_boot_check_skip(BOOT_WIFI, "待连接");
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    ui_boot_check_done();
    ESP_LOGI(TAG, "boot self-check finished");
    vTaskDelete(NULL);
}

#if MAP_SELFTEST
void ui_map_selftest(void);
static void map_selftest_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(20000));
    for (int i = 0; i < 80 && wifi_mgr_get_state() != WIFI_MGR_CONNECTED; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ui_map_selftest();
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    /* 0. NVS（WiFi 凭据 / 相机标定等） */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 恢复上次选择的拍照画幅（默认 1:1）。
     * snap_ar_ver：首次升级到带画幅的版本时清一次旧值，保证默认是 1:1。 */
    {
        nvs_handle_t h;
        if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
            int32_t ver = 0;
            nvs_get_i32(h, "snap_ar_ver", &ver);
            if (ver != 2) {
                nvs_erase_key(h, "snap_ar");
                nvs_set_i32(h, "snap_ar_ver", 2);
                nvs_commit(h);
            } else {
                int32_t ar = 0;
                if (nvs_get_i32(h, "snap_ar", &ar) == ESP_OK && ar >= 0 && ar <= 2) {
                    s_snap_ar = (snap_ar_t)ar;
                }
            }
            int32_t port = 0;
            if (nvs_get_i32(h, "snap_port", &port) == ESP_OK) {
                s_snap_portrait = (port != 0);
            }
            int32_t crot = 180;
            if (nvs_get_i32(h, "cam_rot", &crot) == ESP_OK) {
                s_cam_rot_off = ((crot % 360) + 360) % 360 / 90 * 90;
            }
            nvs_close(h);
        }
    }

    /* 1. 共享 I2C：相机 SCCB / 触摸 / ES8311 共用 GPIO7(SDA)、GPIO8(SCL) */
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN,
        .sda_io_num = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));
    ESP_LOGI(TAG, "shared I2C ready (SDA=%d SCL=%d)",
             CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
             CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN);

    /* 2. 屏幕 + 触摸（触摸失败只降级，不阻断） */
    ESP_ERROR_CHECK(app_lcd_init(&display_panel));
    esp_lcd_touch_handle_t touch = NULL;
    esp_err_t touch_ret = app_touch_init(i2c_bus, &touch);
    if (touch_ret != ESP_OK) {
        ESP_LOGE(TAG, "touch init failed (0x%x), UI without touch", touch_ret);
    }

    /* 3. Shell（LVGL 接管屏幕；此后一切显示都经 LVGL） */
    ui_apps_register_all();
    ESP_ERROR_CHECK(ui_shell_start(display_panel, touch));

    /* 自检：显示 / 触摸已确定，其余项由后台任务补齐 */
    ui_boot_check_mark(BOOT_DISP, true, "480x480");
    ui_boot_check_mark(BOOT_TOUCH, touch != NULL, touch ? "ST7123" : "无触摸");
    xTaskCreate(boot_check_task, "boot_chk", 4096, NULL, 2, NULL);

    /* 5. 网络基础：lwip 与默认事件循环是 socket/RTSP 的前提 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ui_boot_check_mark(BOOT_SYS, true, "网络栈就绪");

    /* 6. Wi-Fi（自动重连上次 WiFi；控制中心可改） */
    esp_err_t net_ret = wifi_mgr_init();
    if (net_ret == ESP_OK) {
        wifi_mgr_set_notify_cb(ui_shell_wifi_notify, NULL);
        /* 天气在后台自己拉，不用先打开 App */
        ui_weather_boot();
    } else {
        ESP_LOGW(TAG, "wifi init failed (0x%x), running offline", net_ret);
    }
    /* 价格提醒的交易日历：先读 NVS 缓存，联网后自动更新 */
    trade_day_init();

    /* 7. RTSP 服务 */
    ESP_ERROR_CHECK(rtsp_server_start(RTSP_PORT));

    /* 8. 音频（失败不阻断） */
    s_audio_ok = (app_audio_init(i2c_bus) == ESP_OK);
    if (!s_audio_ok) {
        ESP_LOGW(TAG, "audio init failed, continue without audio");
    }

    /* 9. 相机 + 编码器 */
    esp_err_t ret = app_video_main(i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video init failed: 0x%x（UI 与 Wi-Fi 仍可用）", ret);
        ui_boot_check_mark(BOOT_CAM, false, "初始化失败");
        xTaskCreate(heartbeat_task, "heartbeat", 3072, NULL, 3, NULL);
        return;
    }

    int video_fd = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT);
    if (video_fd < 0) {
        ESP_LOGE(TAG, "camera open failed");
        ui_boot_check_mark(BOOT_CAM, false, "打开失败");
        xTaskCreate(heartbeat_task, "heartbeat", 3072, (void *)1, 3, NULL);
        return;
    }

    /* 应用用户选择的分辨率/帧率（默认 1280x720@30，与老固件一致） */
    {
        int cw, ch, cf;
        app_video_capture_cfg_get(&cw, &ch, &cf);
        if (cw != 1280 || ch != 720 || cf != 30) {
            if (app_video_set_capture(video_fd, cw, ch, cf) != ESP_OK) {
                ESP_LOGW(TAG, "capture %dx%d@%d 不支持，回退 720P", cw, ch, cf);
                /* 存回默认值，避免下次开机又选到跑不起来的组合（会重启循环） */
                app_video_capture_cfg_set(1280, 720, 30);
                app_video_set_capture(video_fd, 1280, 720, 30);
            }
        }
    }

    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    size_t cam_buf_bytes = 0;
    if (ioctl(video_fd, VIDIOC_G_FMT, &fmt) == 0) {
        if (fmt.fmt.pix.width && fmt.fmt.pix.height) {
            cam_w = fmt.fmt.pix.width;
            cam_h = fmt.fmt.pix.height;
        }
        cam_buf_bytes = fmt.fmt.pix.sizeimage;
        ESP_LOGI(TAG, "camera format: %dx%d, fourcc=0x%08x, buf=%u KB",
                 fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.pixelformat,
                 (unsigned)(cam_buf_bytes / 1024));
    }
    /* YUV420 实际只需 w*h*1.5；app_video_get_buf_size() 按 3 字节/像素算会浪费一倍 */
    if (cam_buf_bytes < (size_t)cam_w * cam_h * 3 / 2) {
        cam_buf_bytes = (size_t)cam_w * cam_h * 3 / 2;
    }

    /* ISP 颜色微调：sensor 节点不支持这些控件，这里不做处理 */
    camera_tune_color(video_fd);

    /*
     * 编码分辨率：直接吃相机原始帧（1280x720，宽高都 16 对齐，硬件编码器要求）。
     * 如果换了非 16 对齐的分辨率，则退回 480x480（PPA 缩放后再编）。
     */
    bool enc_direct = ((cam_w & 15) == 0 && (cam_h & 15) == 0) &&
                      (cam_w <= 1280 && cam_h <= 720);
    ESP_ERROR_CHECK(app_encoder_init(enc_direct ? cam_w : SHELL_PREVIEW_W,
                                     enc_direct ? cam_h : SHELL_PREVIEW_H,
                                     ENC_FPS, ENC_BITRATE_BPS, encoder_out_cb, NULL));
    ESP_LOGI(TAG, "h264: %dx%d %s", enc_direct ? cam_w : SHELL_PREVIEW_W,
             enc_direct ? cam_h : SHELL_PREVIEW_H, enc_direct ? "direct" : "via ppa");

    /* 10. PPA 客户端 */
    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));
    s_ppa_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_ppa_lock ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));

    /* 编码输入：仅在需要 PPA 缩放时分配（480x480 YUV420，约 345KB） */
    yuv_buf_bytes = (size_t)SHELL_PREVIEW_W * SHELL_PREVIEW_H * 3 / 2;
    if (!enc_direct) {
        yuv_buf = heap_caps_aligned_alloc(data_cache_line_size, yuv_buf_bytes, MALLOC_CAP_SPIRAM);
        if (!yuv_buf) {
            ESP_LOGE(TAG, "alloc yuv buffer failed");
            return;
        }
    }

    /* 11. 相机缓冲 + 启动采集 */
    void *camera_buf[EXAMPLE_CAM_BUF_NUM];
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        camera_buf[i] = heap_caps_aligned_calloc(data_cache_line_size, 1,
                                                 cam_buf_bytes, MALLOC_CAP_SPIRAM);
        if (!camera_buf[i]) {
            ESP_LOGE(TAG, "alloc camera buffer %d failed (%u KB)",
                     i, (unsigned)(cam_buf_bytes / 1024));
            return;
        }
    }
    ESP_ERROR_CHECK(app_video_set_bufs(video_fd, EXAMPLE_CAM_BUF_NUM, (void *)camera_buf));

    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(camera_video_frame_operation));
    ESP_ERROR_CHECK(app_video_stream_task_start(video_fd, 0, NULL));

    /* 12. 照片存储挂载放到后台任务：SD 卡初始化可能要几百 ms，不能卡住启动 */
    xTaskCreate(storage_task, "storage", 4096, NULL, 3, NULL);

    xTaskCreate(heartbeat_task, "heartbeat", 3072, touch ? (void *)1 : NULL, 3, NULL);

#if MAP_SELFTEST
    xTaskCreate(map_selftest_task, "map_test", 16384, NULL, 2, NULL);
#endif

#if SNAP_SELFTEST
    xTaskCreate(snapshot_selftest_task, "snap_test", 6144, NULL, 2, NULL);
#endif

    ESP_LOGI(TAG, "system up: rtsp://<device-ip>:%d/%s  （下拉屏幕可配置 Wi-Fi）",
             RTSP_PORT, RTSP_STREAM_PATH);
}

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index,
                                         uint32_t camera_buf_hes, uint32_t camera_buf_ves,
                                         size_t camera_buf_len, void *user_data)
{
    uint64_t now = esp_timer_get_time();

    /* 第一帧到达 → 自检的"相机"项立刻转成通过（即便自检任务已经先报了超时） */
    static bool s_first_frame_reported;
    if (!s_first_frame_reported) {
        s_first_frame_reported = true;
        ui_boot_check_mark(BOOT_CAM, true, "已出图");
        ESP_LOGI(TAG, "first camera frame %ux%u", (unsigned)camera_buf_hes,
                 (unsigned)camera_buf_ves);
    }

    /* --- 取景框跟随拍照画幅：所见即所得 ---
     * 1:1 铺满；其它画幅按比例缩放居中，多余部分留黑边。
     * 每帧都把边条区域清零，否则切回方画幅时会残留上一画幅的画面。 */
    int crop_w, crop_h;
    snap_crop(&crop_w, &crop_h);
    int off_x = (((int)camera_buf_hes - crop_w) / 2) & ~1;   /* YUV420 要求偶数偏移 */
    int off_y = (((int)camera_buf_ves - crop_h) / 2) & ~1;

    int out_w, out_h, out_x, out_y;
    if (crop_w >= crop_h) {
        out_w = SHELL_PREVIEW_W;
        out_h = (int)(((int64_t)crop_h * SHELL_PREVIEW_W / crop_w) & ~1);
        if (out_h > SHELL_PREVIEW_H) out_h = SHELL_PREVIEW_H;
        out_x = 0;
        out_y = (SHELL_PREVIEW_H - out_h) / 2;
    } else {
        out_h = SHELL_PREVIEW_H;
        out_w = (int)(((int64_t)crop_w * SHELL_PREVIEW_H / crop_h) & ~1);
        if (out_w > SHELL_PREVIEW_W) out_w = SHELL_PREVIEW_W;
        out_y = 0;
        out_x = (SHELL_PREVIEW_W - out_w) / 2;
    }

    /* 屏幕转了（纵向/反向）时，取景画面也要跟着转，否则 UI 转正了画面还是横的。
     * 90/270 时输出块的宽高互换，按互换后的尺寸重新居中。 */
    int rot = (ui_shell_camera_rotation() + s_cam_rot_off) % 360;
    int blk_w = (rot == 90 || rot == 270) ? out_h : out_w;
    int blk_h = (rot == 90 || rot == 270) ? out_w : out_h;
    int dst_x = ((SHELL_PREVIEW_W - blk_w) / 2) & ~1;
    int dst_y = ((SHELL_PREVIEW_H - blk_h) / 2) & ~1;
    (void)out_x;
    (void)out_y;

    /* 预览缓冲轮转：PPA 输出到最早未展示的一块 */
    static int s_prev_idx;
    int dst = (s_prev_idx + 1) % SHELL_PREVIEW_NUM;
    s_prev_idx = dst;

    uint8_t *dst_buf = ui_shell_preview_buf(dst);
    /* 先把边条刷黑（只有边条，几十 KB，开销很小） */
    preview_clear_bars(dst_buf, dst_x, dst_y, blk_w, blk_h);

    ppa_srm_oper_config_t srm = {
        .in.buffer = camera_buf,
        .in.pic_w = camera_buf_hes,
        .in.pic_h = camera_buf_ves,
        .in.block_w = crop_w,
        .in.block_h = crop_h,
        .in.block_offset_x = off_x,
        .in.block_offset_y = off_y,
        .in.srm_cm = PPA_SRM_COLOR_MODE_YUV420,
        .in.yuv_range = PPA_COLOR_RANGE_LIMIT,
        .in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,

        .out.buffer = dst_buf,
        .out.buffer_size = ALIGN_UP(SHELL_PREVIEW_W * SHELL_PREVIEW_H * 2, data_cache_line_size),
        .out.pic_w = SHELL_PREVIEW_W,
        .out.pic_h = SHELL_PREVIEW_H,
        .out.block_offset_x = dst_x,
        .out.block_offset_y = dst_y,
        .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,

        .rotation_angle = cam_ppa_angle(rot),
        .scale_x = (float)out_w / (float)crop_w,
        .scale_y = (float)out_h / (float)crop_h,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    xSemaphoreTake(s_ppa_lock, portMAX_DELAY);
    esp_err_t pv = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm);
    xSemaphoreGive(s_ppa_lock);
    if (pv == ESP_OK) {
        /* PPA 是 DMA 写 PSRAM：必须作废 CPU 缓存，否则 LVGL 读到的还是旧像素
         * （表现就是取景画面卡在一帧不动） */
        esp_cache_msync(dst_buf,
                        ALIGN_UP(SHELL_PREVIEW_W * SHELL_PREVIEW_H * 2, data_cache_line_size),
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
        ui_shell_on_frame(dst);
    } else {
        ESP_LOGE(TAG, "ppa preview failed");
    }

    /* --- 快照抓帧：只做一次 memcpy（~2ms），PPA 转换放到拍照任务里做 ---
     * 之前把大尺寸 PPA 放在这里，转换耗时超过一帧间隔，camera_buf 被下一帧
     * DMA 覆盖，照片后半段花屏。复制一份原始帧就彻底避开这个竞争。 */
    if (s_snap_req) {
        s_snap_req = false;
        if (!s_snap_yuv) {
            s_snap_yuv_bytes = (size_t)cam_w * cam_h * 3 / 2;
            s_snap_yuv = heap_caps_aligned_alloc(data_cache_line_size,
                                                 s_snap_yuv_bytes, MALLOC_CAP_SPIRAM);
            if (!s_snap_yuv) {
                ESP_LOGE(TAG, "snapshot yuv alloc failed");
                s_snap_yuv_bytes = 0;
            }
        }
        if (s_snap_yuv) {
            /* 相机写的是 DMA 数据，读之前先作废 CPU 缓存，拿到最新一帧 */
            esp_cache_msync(camera_buf, ALIGN_UP(camera_buf_len, data_cache_line_size),
                            ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
            memcpy(s_snap_yuv, camera_buf,
                   (camera_buf_len < s_snap_yuv_bytes) ? camera_buf_len : s_snap_yuv_bytes);
            s_snap_yuv_ok = true;
        }
    }

    /* --- 推流：16 对齐时直接编码原始帧，否则先缩到 480x480 --- */
    if (!yuv_buf) {
        app_encoder_encode(camera_buf, camera_buf_len, now);
    } else {
        ppa_srm_oper_config_t y = {
            .in.buffer = camera_buf,
            .in.pic_w = camera_buf_hes,
            .in.pic_h = camera_buf_ves,
            .in.block_w = crop_w,
            .in.block_h = crop_h,
            .in.block_offset_x = off_x,
            .in.block_offset_y = off_y,
            .in.srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .in.yuv_range = PPA_COLOR_RANGE_LIMIT,
            .in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,

            .out.buffer = yuv_buf,
            .out.buffer_size = ALIGN_UP(yuv_buf_bytes, data_cache_line_size),
            .out.pic_w = SHELL_PREVIEW_W,
            .out.pic_h = SHELL_PREVIEW_H,
            .out.block_offset_x = 0,
            .out.block_offset_y = 0,
            .out.srm_cm = PPA_SRM_COLOR_MODE_YUV420,

            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = (float)SHELL_PREVIEW_W / (float)crop_w,
            .scale_y = (float)SHELL_PREVIEW_H / (float)crop_h,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        xSemaphoreTake(s_ppa_lock, portMAX_DELAY);
        esp_err_t erc = ppa_do_scale_rotate_mirror(ppa_srm_handle, &y);
        xSemaphoreGive(s_ppa_lock);
        if (erc == ESP_OK) {
            app_encoder_encode(yuv_buf, yuv_buf_bytes, now);
        }
    }
}
