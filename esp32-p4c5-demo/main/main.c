/*
 * ESP32-P4C5 整机 Demo —— 相机预览 + 音频回环
 *
 * 基于 OSPTEK 官方示例（esp32p4-idf5-st7102-mipi-dsi-sc2336-mipi-csi-video-lcd-display）
 * 新增：共享 I2C 总线 + ES8311 音频回环（麦克风 -> 喇叭）。
 *
 * 目标芯片：esp32p4（ESP32-P4C5 核心板）
 */
#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app_video.h"
#include "app_lcd.h"
#include "app_audio.h"

#define ALIGN_UP(num, align)    (((num) + ((align) - 1)) & ~((align) - 1))

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, uint32_t camera_buf_hes, uint32_t camera_buf_ves, size_t camera_buf_len, void *user_data);

static const char *TAG = "app_main";

static esp_lcd_panel_handle_t display_panel;
static ppa_client_handle_t ppa_srm_handle = NULL;
static size_t data_cache_line_size = 0;
static void *lcd_buffer[EXAMPLE_LCD_BUF_NUM];
static i2c_master_bus_handle_t i2c_bus = NULL;

#if CONFIG_EXAMPLE_ENABLE_PRINT_FPS_RATE_VALUE
static int fps_count;
static int64_t start_time;
#endif

void app_main(void)
{
    /* 0. 共享 I2C 总线：相机 SCCB 与 ES8311 共用 GPIO7(SDA)/GPIO8(SCL) */
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN,
        .sda_io_num = CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));
    ESP_LOGI(TAG, "shared I2C bus created (SDA=%d SCL=%d)",
             CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SDA_PIN,
             CONFIG_EXAMPLE_MIPI_CSI_SCCB_I2C_SCL_PIN);

    /* 1. 初始化 LCD（ST7102 480x800 MIPI-DSI） */
    ESP_ERROR_CHECK(app_lcd_init(&display_panel));

    /* 2. 初始化 PPA（用于相机画面缩放/旋转到 LCD） */
    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));

    /* 3. 初始化音频（ES8311 + 功放），复用共享 I2C */
    ESP_ERROR_CHECK(app_audio_init(i2c_bus));

    /* 4. 初始化相机（SC2336，复用共享 I2C） */
    esp_err_t ret = app_video_main(i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video main init failed with error 0x%x", ret);
        return;
    }

    int video_cam_fd0 = app_video_open(EXAMPLE_CAM_DEV_PATH, APP_VIDEO_FMT);
    if (video_cam_fd0 < 0) {
        ESP_LOGE(TAG, "video cam open failed");
        return;
    }

    /* 5. 获取 LCD 帧缓冲 */
#if EXAMPLE_LCD_BUF_NUM == 2
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(display_panel, 2, &lcd_buffer[0], &lcd_buffer[1]));
#else
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(display_panel, 3, &lcd_buffer[0], &lcd_buffer[1], &lcd_buffer[2]));
#endif

    /* 6. 设置相机缓冲 */
#if CONFIG_EXAMPLE_USE_MEMORY_MAPPING
    ESP_LOGI(TAG, "Using map buffer");
    ESP_ERROR_CHECK(app_video_set_bufs(video_cam_fd0, EXAMPLE_CAM_BUF_NUM, NULL));
#else
    ESP_LOGI(TAG, "Using user defined buffer");
    void *camera_buf[EXAMPLE_CAM_BUF_NUM];
    for (int i = 0; i < EXAMPLE_CAM_BUF_NUM; i++) {
        camera_buf[i] = heap_caps_aligned_calloc(data_cache_line_size, 1, app_video_get_buf_size(), MALLOC_CAP_SPIRAM);
    }
    ESP_ERROR_CHECK(app_video_set_bufs(video_cam_fd0, EXAMPLE_CAM_BUF_NUM, (void *)camera_buf));
#endif

    /* 7. 注册帧处理回调（相机 -> PPA -> LCD） */
    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(camera_video_frame_operation));

    /* 8. 启动相机采集任务 */
    ESP_ERROR_CHECK(app_video_stream_task_start(video_cam_fd0, 0, NULL));

    /* 9. 启动音频回环任务（麦克风 -> 喇叭） */
    xTaskCreatePinnedToCore(app_audio_loopback_task, "audio_loopback", 4096, NULL, 5, NULL, 1);

#if CONFIG_EXAMPLE_ENABLE_PRINT_FPS_RATE_VALUE
    start_time = esp_timer_get_time();
#endif
}

static void camera_video_frame_operation(uint8_t *camera_buf, uint8_t camera_buf_index, uint32_t camera_buf_hes, uint32_t camera_buf_ves, size_t camera_buf_len, void *user_data)
{
#if CONFIG_EXAMPLE_ENABLE_PRINT_FPS_RATE_VALUE
    fps_count++;
    if (fps_count == 50) {
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "fps: %f", 1000000.0 / ((end_time - start_time) / 50.0));
        start_time = end_time;
        fps_count = 0;
        ESP_LOGI(TAG, "camera_buf_hes: %lu, camera_buf_ves: %lu, camera_buf_len: %d KB", camera_buf_hes, camera_buf_ves, camera_buf_len / 1024);
    }
#endif

    ppa_srm_oper_config_t srm_config = {
        .in.buffer = camera_buf,
        .in.pic_w = camera_buf_hes,
        .in.pic_h = camera_buf_ves,
        .in.block_w = camera_buf_hes,
        .in.block_h = camera_buf_ves,
        .in.block_offset_x = (camera_buf_hes > EXAMPLE_LCD_H_RES) ? (camera_buf_hes - EXAMPLE_LCD_H_RES) / 2 : 0,
        .in.block_offset_y = (camera_buf_ves > EXAMPLE_LCD_V_RES) ? (camera_buf_ves - EXAMPLE_LCD_V_RES) / 2 : 0,
        .in.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,
        .out.buffer = lcd_buffer[camera_buf_index],
        .out.buffer_size = ALIGN_UP(EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3), data_cache_line_size),
        .out.pic_w = EXAMPLE_LCD_H_RES,
        .out.pic_h = EXAMPLE_LCD_V_RES,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1,
        .scale_y = 1,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    if (camera_buf_hes > EXAMPLE_LCD_H_RES || camera_buf_ves > EXAMPLE_LCD_V_RES) {
        srm_config.in.block_w = (camera_buf_hes > EXAMPLE_LCD_H_RES) ? EXAMPLE_LCD_H_RES : camera_buf_hes;
        srm_config.in.block_h = (camera_buf_ves > EXAMPLE_LCD_V_RES) ? EXAMPLE_LCD_V_RES : camera_buf_ves;
        ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config));
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(display_panel, 0, 0, srm_config.in.block_w, srm_config.in.block_h, lcd_buffer[camera_buf_index]));
    } else {
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(display_panel, 0, 0, camera_buf_hes, camera_buf_ves, camera_buf));
    }
}
