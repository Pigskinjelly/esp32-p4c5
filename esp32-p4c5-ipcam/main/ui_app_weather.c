/*
 * 天气 App —— 多城市实时天气 + 未来 3 天预报
 *
 * 数据源：open-meteo.com（免费、免 API Key、HTTPS JSON）
 *   current: 温度/体感/湿度/风速/天气代码(WMO)
 *   daily:   最高/最低/天气代码 × 4 天
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "cJSON.h"
#include "nvs.h"

#include "lvgl.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "ui_apps.h"
#include "wifi_mgr.h"

static const char *TAG = "ui_weather";

/* ================= 城市表 ================= */

typedef struct {
    const char *name;
    float lat;
    float lon;
} city_t;

static const city_t s_cities[] = {
    { "北京", 39.9042f, 116.4074f },
    { "上海", 31.2304f, 121.4737f },
    { "广州", 23.1291f, 113.2644f },
    { "深圳", 22.5431f, 114.0579f },
    { "杭州", 30.2741f, 120.1551f },
    { "成都", 30.5728f, 104.0668f },
    { "武汉", 30.5928f, 114.3055f },
    { "西安", 34.3416f, 108.9398f },
    { "南京", 32.0603f, 118.7969f },
    { "重庆", 29.5630f, 106.5516f },
};
#define CITY_COUNT (int)(sizeof(s_cities) / sizeof(s_cities[0]))

static int s_city_idx;

/* ================= 天气数据 ================= */

typedef struct {
    bool  valid;
    float temp;
    float feels;
    int   humidity;
    float wind;
    int   code;
    struct {
        int   code;
        float tmax;
        float tmin;
    } daily[4];
    int   daily_cnt;
    char  updated[16];
} weather_t;

static weather_t s_wx;
static volatile bool s_dirty;
static volatile bool s_busy;
static SemaphoreHandle_t s_sem;

/* WMO 天气代码 → 中文 */
static const char *wmo_text(int code)
{
    if (code == 0) return "晴";
    if (code == 1) return "晴间多云";
    if (code == 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 57) return "毛毛雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code == 85 || code == 86) return "阵雪";
    if (code >= 95) return "雷阵雨";
    return "未知";
}

/* 天气冷暖色调：晴暖色、雨雪冷色 */
static uint32_t wmo_color(int code)
{
    if (code == 0 || code == 1) return 0xFFC44D;     /* 晴 */
    if (code == 2) return 0x9FC4E8;
    if (code == 3) return 0x8FA8CC;
    if (code == 45 || code == 48) return 0xB0B8C4;
    if (code >= 71 && code <= 77) return 0x9FE8FF;   /* 雪 */
    if (code >= 95) return 0xFF9A5C;                 /* 雷 */
    return 0x6FC4FF;                                 /* 雨 */
}

/* ================= 网络抓取 ================= */

static void parse_json(const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ESP_LOGW(TAG, "json parse failed");
        return;
    }

    weather_t wx = { 0 };
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        cJSON *it;
        if ((it = cJSON_GetObjectItem(cur, "temperature_2m")))      wx.temp = (float)it->valuedouble;
        if ((it = cJSON_GetObjectItem(cur, "apparent_temperature"))) wx.feels = (float)it->valuedouble;
        if ((it = cJSON_GetObjectItem(cur, "relative_humidity_2m"))) wx.humidity = it->valueint;
        if ((it = cJSON_GetObjectItem(cur, "wind_speed_10m")))       wx.wind = (float)it->valuedouble;
        if ((it = cJSON_GetObjectItem(cur, "weather_code")))         wx.code = it->valueint;
        wx.valid = true;
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily) {
        cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
        cJSON *tmax = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *tmin = cJSON_GetObjectItem(daily, "temperature_2m_min");
        int n = cJSON_GetArraySize(codes);
        if (n > 4) n = 4;
        for (int i = 0; i < n; i++) {
            cJSON *c = cJSON_GetArrayItem(codes, i);
            cJSON *mx = cJSON_GetArrayItem(tmax, i);
            cJSON *mn = cJSON_GetArrayItem(tmin, i);
            if (!c || !mx || !mn) break;
            wx.daily[wx.daily_cnt].code = c->valueint;
            wx.daily[wx.daily_cnt].tmax = (float)mx->valuedouble;
            wx.daily[wx.daily_cnt].tmin = (float)mn->valuedouble;
            wx.daily_cnt++;
        }
    }

    /* 更新时间（本地时间） */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    snprintf(wx.updated, sizeof(wx.updated), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);

    cJSON_Delete(root);

    if (wx.valid) {
        s_wx = wx;
        s_dirty = true;
        ESP_LOGI(TAG, "weather updated: %s %.1fC code=%d daily=%d",
                 s_cities[s_city_idx].name, wx.temp, wx.code, wx.daily_cnt);
    }
}

static void fetch_task(void *arg)
{
    (void)arg;
    int64_t last_ok_us = 0;

    while (1) {
        /* 还没拿到过数据时每 2 秒重试一次：开机时 WiFi 要十几秒才连上，
         * 原来固定等 30 秒才会再拉，锁屏天气因此一直空着 */
        TickType_t wait = (last_ok_us == 0) ? pdMS_TO_TICKS(2000) : pdMS_TO_TICKS(30000);
        bool forced = (xSemaphoreTake(s_sem, wait) == pdTRUE);

        bool stale = (last_ok_us == 0) ||
                     (esp_timer_get_time() - last_ok_us > 10LL * 60 * 1000000);
        if (!forced && !stale) {
            continue;
        }
        if (wifi_mgr_get_state() != WIFI_MGR_CONNECTED) {
            s_busy = false;
            continue;
        }
        char url[320];
        snprintf(url, sizeof(url),
                 "https://api.open-meteo.com/v1/forecast"
                 "?latitude=%.4f&longitude=%.4f"
                 "&current=temperature_2m,relative_humidity_2m,apparent_temperature,"
                 "weather_code,wind_speed_10m"
                 "&daily=weather_code,temperature_2m_max,temperature_2m_min"
                 "&timezone=Asia%%2FShanghai&forecast_days=4",
                 s_cities[s_city_idx].lat, s_cities[s_city_idx].lon);

        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 10000,
            .buffer_size = 4096,
            .user_agent = "ESP32-P4C5-Weather/1.0",
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            esp_err_t err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(client);
                int status = esp_http_client_get_status_code(client);
                static char body[6144];
                int total = 0;
                while (total < (int)sizeof(body) - 1) {
                    int r = esp_http_client_read(client, body + total, sizeof(body) - 1 - total);
                    if (r <= 0) break;
                    total += r;
                }
                body[total] = '\0';
                if (total > 0 && status == 200) {
                    parse_json(body);
                    if (s_wx.valid) {
                        last_ok_us = esp_timer_get_time();
                    }
                } else {
                    ESP_LOGW(TAG, "fetch failed: http %d, %d bytes", status, total);
                }
            } else {
                ESP_LOGW(TAG, "http open failed: 0x%x", err);
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
        }
        s_busy = false;
    }
}

static void fetch_now(void)
{
    if (!s_busy && s_sem) {
        s_busy = true;
        xSemaphoreGive(s_sem);
    }
}

/* 启动抓取任务（开机时调用一次，App 首次打开也会兜底） */
static void weather_start(void)
{
    if (s_sem) {
        return;
    }
    nvs_handle_t h;
    int32_t c = 0;
    if (nvs_open("sys", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "city", &c) == ESP_OK && c >= 0 && c < CITY_COUNT) {
            s_city_idx = c;
        }
        nvs_close(h);
    }
    /* 只起一次：以前每次 create 都新建任务，进出几次就吃掉几十 KB 内部 RAM */
    static bool started;
    if (!started) {
        started = true;
        s_sem = xSemaphoreCreateBinary();
        xTaskCreateWithCaps(fetch_task, "wx_fetch", 8192, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    }
}

/* 开机联网后自动获取（不依赖打开 App） */
void ui_weather_boot(void)
{
    /* 天气 App 在设置里被关掉时，连后台抓取也不做（省流量/省电） */
    if (!ui_shell_app_on_by_name("天气")) {
        ESP_LOGI(TAG, "weather app disabled, skip boot fetch");
        return;
    }
    weather_start();
    fetch_now();
}

/* ================= UI ================= */

static lv_obj_t *s_lbl_city;
static lv_obj_t *s_lbl_temp;
static lv_obj_t *s_lbl_desc;
static lv_obj_t *s_lbl_feels;
static lv_obj_t *s_lbl_hum;
static lv_obj_t *s_lbl_wind;
static lv_obj_t *s_lbl_updated;
static lv_obj_t *s_lbl_date;
static lv_obj_t *s_lbl_hum2;
static lv_obj_t *s_lbl_wind2;
static lv_obj_t *s_lbl_air;
static lv_obj_t *s_lbl_daily[3];
static lv_obj_t *s_lbl_daily_desc[3];
static lv_obj_t *s_lbl_daily_temp[3];

static void icon_apply(int code);   /* 动态图标（定义在后面） */

static void apply_ui(void)
{
    lv_label_set_text(s_lbl_city, s_cities[s_city_idx].name);

    /* 日期 */
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2026) {
        static const char *wd[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
        lv_label_set_text_fmt(s_lbl_date, "%04d-%02d-%02d  %s",
                              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, wd[t.tm_wday]);
    } else {
        lv_label_set_text(s_lbl_date, "----");
    }

    if (!s_wx.valid) {
        lv_label_set_text(s_lbl_temp, "--");
        lv_label_set_text(s_lbl_desc, wifi_mgr_get_state() == WIFI_MGR_CONNECTED ?
                          "正在获取天气…" : "未连接网络");
        if (s_lbl_hum2) lv_label_set_text(s_lbl_hum2, "--");
        if (s_lbl_wind2) lv_label_set_text(s_lbl_wind2, "--");
        if (s_lbl_air) lv_label_set_text(s_lbl_air, "--");
        if (s_lbl_updated) lv_label_set_text(s_lbl_updated, "");
        for (int i = 0; i < 3; i++) {
            if (s_lbl_daily_desc[i]) lv_label_set_text(s_lbl_daily_desc[i], "--");
            if (s_lbl_daily_temp[i]) lv_label_set_text(s_lbl_daily_temp[i], "-- / --");
        }
        return;
    }

    lv_label_set_text_fmt(s_lbl_temp, "%.0f", s_wx.temp);
    lv_label_set_text(s_lbl_desc, wmo_text(s_wx.code));
    lv_obj_set_style_text_color(s_lbl_desc, col(wmo_color(s_wx.code)), 0);
    lv_obj_set_style_text_color(s_lbl_temp, col(wmo_color(s_wx.code)), 0);

    if (s_lbl_hum2) lv_label_set_text_fmt(s_lbl_hum2, "%d%%", s_wx.humidity);
    if (s_lbl_wind2) lv_label_set_text_fmt(s_lbl_wind2, "%.0f km/h", s_wx.wind);
    if (s_lbl_air) lv_label_set_text_fmt(s_lbl_air, "%.0f度", s_wx.feels);
    if (s_lbl_updated) lv_label_set_text_fmt(s_lbl_updated, "更新于 %s", s_wx.updated);
    icon_apply(s_wx.code);

    for (int i = 0; i < 3; i++) {
        if (i < s_wx.daily_cnt && s_lbl_daily_desc[i]) {
            lv_label_set_text(s_lbl_daily_desc[i], wmo_text(s_wx.daily[i].code));
            lv_obj_set_style_text_color(s_lbl_daily_desc[i], col(wmo_color(s_wx.daily[i].code)), 0);
            lv_label_set_text_fmt(s_lbl_daily_temp[i], "%.0f / %.0f",
                                  s_wx.daily[i].tmax, s_wx.daily[i].tmin);
        }
    }
}

static void prev_city_cb(lv_event_t *e)
{
    (void)e;
    s_city_idx = (s_city_idx + CITY_COUNT - 1) % CITY_COUNT;
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "city", s_city_idx);
        nvs_commit(h);
        nvs_close(h);
    }
    s_wx.valid = false;
    apply_ui();
    fetch_now();
}

static void next_city_cb(lv_event_t *e)
{
    (void)e;
    s_city_idx = (s_city_idx + 1) % CITY_COUNT;
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "city", s_city_idx);
        nvs_commit(h);
        nvs_close(h);
    }
    s_wx.valid = false;
    apply_ui();
    fetch_now();
}

/* 供锁屏使用的天气摘要 */
bool ui_weather_summary(float *temp, float *tmax, float *tmin, int *code, const char **city)
{
    if (!s_wx.valid) {
        return false;
    }
    if (temp) *temp = s_wx.temp;
    if (tmax) *tmax = (s_wx.daily_cnt > 0) ? s_wx.daily[0].tmax : s_wx.temp;
    if (tmin) *tmin = (s_wx.daily_cnt > 0) ? s_wx.daily[0].tmin : s_wx.temp;
    if (code) *code = s_wx.code;
    if (city) *city = s_cities[s_city_idx].name;
    return true;
}

const char *ui_weather_desc(void)
{
    return s_wx.valid ? wmo_text(s_wx.code) : "";
}

/* ================= 3D 风格天气画面 ================= */

static lv_obj_t *s_art_box;
static lv_obj_t *s_sun_rays;
static lv_obj_t *s_cloud;
static lv_obj_t *s_drop[3];

static void anim_rot_exec(void *obj, int32_t v)
{
    lv_obj_set_style_transform_rotation((lv_obj_t *)obj, (int32_t)v, 0);
}

static void anim_x_exec(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

static void anim_yoff_exec(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y((lv_obj_t *)obj, v, 0);
}

static void anim_repeat(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from,
                        int32_t to, uint32_t dur, int32_t delay, bool playback)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, dur);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    if (playback) {
        lv_anim_set_playback_time(&a, dur);
        lv_anim_set_playback_delay(&a, 0);
    }
    lv_anim_start(&a);
}

/* 圆/胶囊形色块，可选上下渐变做立体感 */
static lv_obj_t *blob(lv_obj_t *parent, int w, int h, int x, int y,
                      uint32_t c1, uint32_t c2, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, col(c1), 0);
    lv_obj_set_style_bg_grad_color(o, col(c2), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* 参考图那种「玻璃感云 + 蓝色雨滴」 */
static void art_build(lv_obj_t *parent)
{
    s_art_box = lv_obj_create(parent);
    lv_obj_set_size(s_art_box, 168, 150);
    lv_obj_set_pos(s_art_box, 284, 40);
    lv_obj_set_style_bg_opa(s_art_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_art_box, 0, 0);
    lv_obj_set_style_pad_all(s_art_box, 0, 0);
    lv_obj_clear_flag(s_art_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_art_box, LV_OBJ_FLAG_CLICKABLE);

    /* 太阳（晴天时露出，光芒慢转） */
    s_sun_rays = lv_obj_create(s_art_box);
    lv_obj_set_size(s_sun_rays, 92, 92);
    lv_obj_align(s_sun_rays, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_bg_opa(s_sun_rays, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sun_rays, 0, 0);
    lv_obj_set_style_pad_all(s_sun_rays, 0, 0);
    lv_obj_clear_flag(s_sun_rays, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_sun_rays, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_transform_pivot_x(s_sun_rays, 46, 0);
    lv_obj_set_style_transform_pivot_y(s_sun_rays, 46, 0);
    for (int i = 0; i < 8; i++) {
        lv_obj_t *ray = blob(s_sun_rays, 6, 16, 43, 0, 0xFFC24A, 0xFFE08A, 3);
        lv_obj_set_style_transform_pivot_x(ray, 3, 0);
        lv_obj_set_style_transform_pivot_y(ray, 46, 0);
        lv_obj_set_style_transform_rotation(ray, (int32_t)(i * 450), 0);
    }
    anim_repeat(s_sun_rays, anim_rot_exec, 0, 3600, 18000, 0, false);
    lv_obj_t *sun = blob(s_art_box, 66, 66, 51, 18, 0xFFE066, 0xFFB020, LV_RADIUS_CIRCLE);
    lv_obj_set_style_shadow_width(sun, 28, 0);
    lv_obj_set_style_shadow_color(sun, col(0xFFB020), 0);
    lv_obj_set_style_shadow_opa(sun, LV_OPA_40, 0);

    /* 云（会左右轻飘） */
    s_cloud = lv_obj_create(s_art_box);
    lv_obj_set_size(s_cloud, 168, 108);
    lv_obj_set_pos(s_cloud, 0, 18);
    lv_obj_set_style_bg_opa(s_cloud, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cloud, 0, 0);
    lv_obj_set_style_pad_all(s_cloud, 0, 0);
    lv_obj_clear_flag(s_cloud, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_cloud, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *c1 = blob(s_cloud, 58, 58, 16, 26, 0xF3EEFF, 0xCFC6EC, LV_RADIUS_CIRCLE);
    lv_obj_t *c2 = blob(s_cloud, 82, 82, 50, 8, 0xFBF7FF, 0xD8CFF2, LV_RADIUS_CIRCLE);
    lv_obj_t *c3 = blob(s_cloud, 54, 54, 108, 28, 0xEFE9FB, 0xC8BFE8, LV_RADIUS_CIRCLE);
    lv_obj_t *c4 = blob(s_cloud, 140, 40, 16, 62, 0xF7F2FF, 0xD5CCF0, 20);
    lv_obj_t *all[4] = { c1, c2, c3, c4 };
    for (int i = 0; i < 4; i++) {
        lv_obj_set_style_shadow_width(all[i], 26, 0);
        lv_obj_set_style_shadow_color(all[i], col(0x8E7BFF), 0);
        lv_obj_set_style_shadow_opa(all[i], LV_OPA_30, 0);
    }
    anim_repeat(s_cloud, anim_x_exec, -4, 6, 2600, 0, true);

    /* 雨滴 */
    for (int i = 0; i < 3; i++) {
        s_drop[i] = blob(s_art_box, 9, 24, 52 + i * 26, 100, 0x3B5BFF, 0x2233CC, 5);
        lv_obj_set_style_transform_rotation(s_drop[i], 120, 0);
        anim_repeat(s_drop[i], anim_yoff_exec, -12, 14, 900, i * 240, false);
    }
}

static void icon_apply(int code)
{
    if (!s_art_box) {
        return;
    }
    bool rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82) ||
                (code >= 95 && code <= 99);
    bool snow = (code >= 71 && code <= 77) || (code >= 85 && code <= 86);
    bool clear = (code == 0 || code == 1);
    bool partly = (code == 2);

    if (clear || partly) {
        lv_obj_remove_flag(s_sun_rays, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_sun_rays, LV_OBJ_FLAG_HIDDEN);
    }
    if (clear) {
        lv_obj_add_flag(s_cloud, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_cloud, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 3; i++) {
        if (rain || snow) {
            lv_obj_remove_flag(s_drop[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_drop[i], col(snow ? 0xE8F1FF : 0x3B5BFF), 0);
        } else {
            lv_obj_add_flag(s_drop[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ================= 界面 ================= */

/* 一行：小图标 + 名称 + 值 */
/* 一项：色点 + 名称 + 数值（不用字体符号，避免缺字形显示成方框） */
static void info_item(lv_obj_t *parent, int x, int y, uint32_t dot_color,
                      const char *name, lv_obj_t **val_out)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_pos(dot, x, y + 6);
    lv_obj_set_style_bg_color(dot, col(dot_color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *nm = ui_label(parent, name, &ui_cjk_16, 0x7B8CA6);
    lv_obj_set_pos(nm, x + 16, y - 1);
    lv_obj_t *v = ui_label(parent, "--", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(v, x + 16, y + 21);
    *val_out = v;
}

/* 每次重建界面前清空控件指针。
 * App 没开"后台"时退出会销毁整个界面，静态指针就此悬空；
 * 下一次 create 里若还有函数先用到它（比如按旧按钮改样式），
 * 就会 Load access fault 直接重启。 */
static void reset_ui_refs(void)
{
    s_lbl_city = NULL;
    s_lbl_temp = NULL;
    s_lbl_desc = NULL;
    s_lbl_feels = NULL;
    s_lbl_hum = NULL;
    s_lbl_wind = NULL;
    s_lbl_updated = NULL;
    s_lbl_date = NULL;
    s_lbl_hum2 = NULL;
    s_lbl_wind2 = NULL;
    s_lbl_air = NULL;
    memset(s_lbl_daily, 0, sizeof(s_lbl_daily));
    memset(s_lbl_daily_desc, 0, sizeof(s_lbl_daily_desc));
    memset(s_lbl_daily_temp, 0, sizeof(s_lbl_daily_temp));
    s_art_box = NULL;
    s_sun_rays = NULL;
    s_cloud = NULL;
    memset(s_drop, 0, sizeof(s_drop));
}

static void create(lv_obj_t *parent)
{
    reset_ui_refs();
    ui_app_topbar(parent, "天气");
    lv_obj_set_style_bg_color(parent, col(0x0B0E14), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* ---- 城市切换 ---- */
    lv_obj_t *btn_prev = lv_button_create(parent);
    lv_obj_set_size(btn_prev, 40, 32);
    lv_obj_set_pos(btn_prev, 12, 38);
    lv_obj_set_style_bg_color(btn_prev, col(0x1A2230), 0);
    lv_obj_set_style_radius(btn_prev, 16, 0);
    lv_obj_add_event_cb(btn_prev, prev_city_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pl = ui_label(btn_prev, "<", &lv_font_montserrat_20, C_TXT);
    lv_obj_center(pl);

    lv_obj_t *btn_next = lv_button_create(parent);
    lv_obj_set_size(btn_next, 40, 32);
    lv_obj_set_pos(btn_next, 428, 38);
    lv_obj_set_style_bg_color(btn_next, col(0x1A2230), 0);
    lv_obj_set_style_radius(btn_next, 16, 0);
    lv_obj_add_event_cb(btn_next, next_city_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nl = ui_label(btn_next, ">", &lv_font_montserrat_20, C_TXT);
    lv_obj_center(nl);

    s_lbl_city = ui_label(parent, "深圳", &ui_cjk_16, C_TXT);
    lv_obj_align(s_lbl_city, LV_ALIGN_TOP_MID, 0, 38);

    /* ---- 主卡片 ---- */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 460, 286);
    lv_obj_set_pos(card, 10, 78);
    lv_obj_set_style_bg_color(card, col(0x161A22), 0);
    lv_obj_set_style_bg_grad_color(card, col(0x1E2431), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(card, 235, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, col(0x2A3345), 0);
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *today = ui_label(card, "今日天气", &ui_cjk_16, 0x7B8CA6);
    lv_obj_set_pos(today, 22, 16);

    s_lbl_temp = lv_label_create(card);
    lv_label_set_text(s_lbl_temp, "--");
    lv_obj_set_style_text_font(s_lbl_temp, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_lbl_temp, col(C_TXT), 0);
    lv_obj_set_pos(s_lbl_temp, 18, 40);

    lv_obj_t *unit = ui_label(card, "°C", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(unit, 18 + 74, 52);

    s_lbl_desc = ui_label(card, "—", &ui_cjk_16, 0x9FC4E8);
    lv_obj_set_pos(s_lbl_desc, 20, 96);

    s_lbl_date = ui_label(card, "--", &ui_cjk_16, 0x7B8CA6);
    lv_obj_set_pos(s_lbl_date, 20, 120);

    /* 右侧 3D 云 */
    art_build(card);

    /* 分隔线 */
    lv_obj_t *l1 = lv_obj_create(card);
    lv_obj_set_size(l1, 416, 1);
    lv_obj_set_pos(l1, 22, 206);
    lv_obj_set_style_bg_color(l1, col(0x2A3345), 0);
    lv_obj_set_style_bg_opa(l1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(l1, 0, 0);
    lv_obj_set_style_radius(l1, 0, 0);
    lv_obj_clear_flag(l1, LV_OBJ_FLAG_SCROLLABLE);

    /* 空气 / 风向 / 湿度 */
    info_item(card, 24, 214, 0x5AA9FF, "湿度", &s_lbl_hum2);
    info_item(card, 178, 214, 0x7BD6A6, "风向", &s_lbl_wind2);
    info_item(card, 330, 214, 0xFFC46B, "体感", &s_lbl_air);

    /* ---- 未来三天 ---- */
    lv_obj_t *fc = lv_obj_create(parent);
    lv_obj_set_size(fc, 460, 96);
    lv_obj_set_pos(fc, 10, 372);
    lv_obj_set_style_bg_color(fc, col(0x161A22), 0);
    lv_obj_set_style_bg_opa(fc, 225, 0);
    lv_obj_set_style_border_width(fc, 1, 0);
    lv_obj_set_style_border_color(fc, col(0x2A3345), 0);
    lv_obj_set_style_radius(fc, 22, 0);
    lv_obj_set_style_pad_all(fc, 0, 0);
    lv_obj_clear_flag(fc, LV_OBJ_FLAG_SCROLLABLE);

    static const char *dn[3] = { "今天", "明天", "后天" };
    for (int i = 0; i < 3; i++) {
        int cx = 24 + i * 152;
        s_lbl_daily[i] = ui_label(fc, dn[i], &ui_cjk_16, 0x7B8CA6);
        lv_obj_set_pos(s_lbl_daily[i], cx, 12);

        s_lbl_daily_desc[i] = ui_label(fc, "--", &ui_cjk_16, 0x9FC4E8);
        lv_obj_set_pos(s_lbl_daily_desc[i], cx, 38);

        s_lbl_daily_temp[i] = ui_label(fc, "--", &ui_cjk_16, C_TXT);
        lv_obj_set_pos(s_lbl_daily_temp[i], cx, 62);
    }

    s_lbl_updated = ui_label(card, "", NULL, 0x5B6B85);
    lv_obj_align(s_lbl_updated, LV_ALIGN_TOP_RIGHT, -20, 16);

    /* 读上次选择的城市 + 启动抓取任务 */
    weather_start();
    apply_ui();
}

static void tick(void)
{
    if (s_dirty) {
        s_dirty = false;
        apply_ui();
    }
    /* 每 10 分钟自动刷新一次 */
    static int cnt;
    if (++cnt >= 1200) {      /* tick 500ms × 1200 = 10 分钟 */
        cnt = 0;
        fetch_now();
    }
}

static void on_show(void)
{
    ui_shell_set_tick_cb(tick);
    if (!s_wx.valid) {
        apply_ui();
    }
    fetch_now();
}

static void on_hide(void)
{
    ui_shell_set_tick_cb(NULL);
}

const ui_app_t ui_app_weather = {
    .name = "天气",
    .symbol = LV_SYMBOL_TINT,
    .color = 0x1B7A8C,
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
