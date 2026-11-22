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
#include "esp_heap_caps.h"
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
    int   aqi;               /* 美标 AQI，0 = 未知 */
    int   humidity;
    float wind;
    int   code;
    struct {
        int   code;
        float tmax;
        float tmin;
    } daily[4];
    int   daily_cnt;
    struct {                 /* 逐小时（参考图的"现在 / 19时…"一排） */
        int   code;
        float t;
        char  hh[4];         /* "15" */
    } hourly[6];
    int   hourly_cnt;
    char  cur_hour[4];
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
    wx.aqi = s_wx.aqi;          /* 空气质量是另一个接口，先沿用上次的值 */
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

    /* 逐小时：取前 5 条（forecast_hours=6，从当前小时开始） */
    cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (hourly) {
        cJSON *times = cJSON_GetObjectItem(hourly, "time");
        cJSON *temps = cJSON_GetObjectItem(hourly, "temperature_2m");
        cJSON *codes = cJSON_GetObjectItem(hourly, "weather_code");
        int n = cJSON_GetArraySize(times);
        for (int i = 0; i < n && wx.hourly_cnt < 5; i++) {
            cJSON *ts = cJSON_GetArrayItem(times, i);
            cJSON *tp = cJSON_GetArrayItem(temps, i);
            cJSON *cd = cJSON_GetArrayItem(codes, i);
            if (!ts || !tp || !cd || !cJSON_IsString(ts)) {
                break;
            }
            /* "2026-11-22T15:00" → hh = "15" */
            const char *str = ts->valuestring;
            const char *tpos = strchr(str, 'T');
            wx.hourly[wx.hourly_cnt].code = cd->valueint;
            wx.hourly[wx.hourly_cnt].t = (float)tp->valuedouble;
            if (tpos && strlen(tpos + 1) >= 2) {
                memcpy(wx.hourly[wx.hourly_cnt].hh, tpos + 1, 2);
                wx.hourly[wx.hourly_cnt].hh[2] = '\0';
            } else {
                strlcpy(wx.hourly[wx.hourly_cnt].hh, "--", 4);
            }
            wx.hourly_cnt++;
        }
    }

    /* 更新时间（本地时间） */
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    snprintf(wx.updated, sizeof(wx.updated), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
    snprintf(wx.cur_hour, sizeof(wx.cur_hour), "%02d", tm_info.tm_hour);

    cJSON_Delete(root);

    if (wx.valid) {
        s_wx = wx;
        s_dirty = true;
        ESP_LOGI(TAG, "weather updated: %s %.1fC code=%d daily=%d hourly=%d",
                 s_cities[s_city_idx].name, wx.temp, wx.code, wx.daily_cnt,
                 wx.hourly_cnt);
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
                 "&hourly=temperature_2m,weather_code&forecast_hours=6"
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
                static char body[9216];
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
        /* 空气质量（可选）：拿不到就保持 --，不影响主流程 */
        if (s_wx.valid) {
            char aurl[256];
            snprintf(aurl, sizeof(aurl),
                     "https://air-quality-api.open-meteo.com/v1/air-quality"
                     "?latitude=%.4f&longitude=%.4f&current=us_aqi"
                     "&timezone=Asia%%2FShanghai",
                     s_cities[s_city_idx].lat, s_cities[s_city_idx].lon);
            esp_http_client_config_t acfg = {
                .url = aurl,
                .timeout_ms = 8000,
                .buffer_size = 1024,
                .user_agent = "ESP32-P4C5-Weather/1.0",
                .crt_bundle_attach = esp_crt_bundle_attach,
            };
            esp_http_client_handle_t ac = esp_http_client_init(&acfg);
            if (ac) {
                static char ab[1024];
                int an = 0;
                if (esp_http_client_open(ac, 0) == ESP_OK) {
                    esp_http_client_fetch_headers(ac);
                    if (esp_http_client_get_status_code(ac) == 200) {
                        while (an < (int)sizeof(ab) - 1) {
                            int r = esp_http_client_read(ac, ab + an, sizeof(ab) - 1 - an);
                            if (r <= 0) break;
                            an += r;
                        }
                        ab[an] = '\0';
                        cJSON *root = cJSON_Parse(ab);
                        if (root) {
                            cJSON *cur = cJSON_GetObjectItem(root, "current");
                            cJSON *aq = cur ? cJSON_GetObjectItem(cur, "us_aqi") : NULL;
                            if (aq && cJSON_IsNumber(aq)) {
                                s_wx.aqi = aq->valueint;
                                s_dirty = true;
                                ESP_LOGI(TAG, "aqi=%d", s_wx.aqi);
                            }
                            cJSON_Delete(root);
                        }
                    }
                }
                esp_http_client_close(ac);
                esp_http_client_cleanup(ac);
            }
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
        SemaphoreHandle_t sem = xSemaphoreCreateBinary();
        if (sem) {
            BaseType_t ok = xTaskCreateWithCaps(fetch_task, "wx_fetch", 8192, NULL, 3, NULL,
                                                MALLOC_CAP_SPIRAM);
            if (ok == pdPASS) {
                s_sem = sem;
            } else {
                vSemaphoreDelete(sem);
                ESP_LOGE(TAG, "wx_fetch create failed: 天气不可用");
            }
        }
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

/* ================= 动态背景（时段 + 天气） ================= */

#define WBG_W 480
#define WBG_H 480

static uint16_t *s_bg_buf;
static lv_image_dsc_t s_bg_dsc;
static lv_obj_t *s_bg_img;
static int s_bg_key = -1;

static inline uint16_t c565(int r, int g, int b)
{
    r = r < 0 ? 0 : (r > 255 ? 255 : r);
    g = g < 0 ? 0 : (g > 255 ? 255 : g);
    b = b < 0 ? 0 : (b > 255 ? 255 : b);
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static inline int mix8(int a, int b, int t)
{
    return a + (b - a) * t / 255;
}

/* 时段：0 夜 / 1 黎明 / 2 上午 / 3 白天 / 4 黄昏 / 5 入夜 */
static int day_slot(int hour)
{
    if (hour < 5)  return 0;
    if (hour < 7)  return 1;
    if (hour < 11) return 2;
    if (hour < 16) return 3;
    if (hour < 18) return 4;
    if (hour < 21) return 5;
    return 0;
}

/* 每个时段的 上 / 中 / 下 三色 */
static const uint8_t s_pal[6][3][3] = {
    { {  4,  6, 14}, {  9, 14, 40}, { 24, 32, 62} },      /* 夜 */
    { { 16, 22, 56}, { 76, 52, 94}, { 214, 132, 92} },     /* 黎明 */
    { { 16, 54, 104}, { 56, 120, 168}, { 154, 198, 218} }, /* 上午 */
    { { 20, 76, 138}, { 66, 138, 188}, { 176, 214, 232} }, /* 白天 */
    { { 10, 16, 44}, { 58, 42, 84}, { 196, 104, 64} },     /* 黄昏 */
    { {  7,  9, 28}, { 36, 28, 64}, { 110, 54, 72} },      /* 入夜 */
};

/* 天气对底色的修正（往 tint 混 amt/255） */
static void weather_tint(int code, int *tr, int *tg, int *tb, int *amt)
{
    *tr = 0; *tg = 0; *tb = 0; *amt = 0;
    if (code < 0) {
        return;
    }
    if (code == 0) { *tr = 255; *tg = 214; *tb = 150; *amt = 14; }        /* 晴 */
    else if (code == 1) { *tr = 210; *tg = 220; *tb = 240; *amt = 16; }   /* 晴间多云 */
    else if (code == 2) { *tr = 150; *tg = 165; *tb = 190; *amt = 26; }   /* 多云 */
    else if (code == 3) { *tr = 118; *tg = 126; *tb = 142; *amt = 42; }   /* 阴 */
    else if (code == 45 || code == 48) { *tr = 170; *tg = 176; *tb = 184; *amt = 52; }
    else if (code >= 71 && code <= 77) { *tr = 205; *tg = 220; *tb = 238; *amt = 46; } /* 雪 */
    else if (code >= 95) { *tr = 58; *tg = 34; *tb = 74; *amt = 52; }     /* 雷 */
    else { *tr = 42; *tg = 62; *tb = 92; *amt = 38; }                     /* 雨 */
}

/* 背景重绘：480 行渐变（每行算一次色，带一点上下压暗） */
static void bg_render(void)
{
    if (!s_bg_buf) {
        return;
    }
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    int slot = day_slot(t.tm_hour);
    int tr, tg, tb, amt;
    weather_tint(s_wx.valid ? s_wx.code : -1, &tr, &tg, &tb, &amt);

    const uint8_t *c0 = s_pal[slot][0];
    const uint8_t *c1 = s_pal[slot][1];
    const uint8_t *c2 = s_pal[slot][2];

    for (int y = 0; y < WBG_H; y++) {
        int tt = y * 255 / (WBG_H - 1);
        int r, g, b;
        if (tt < 128) {
            int k = tt * 2;
            r = mix8(c0[0], c1[0], k);
            g = mix8(c0[1], c1[1], k);
            b = mix8(c0[2], c1[2], k);
        } else {
            int k = (tt - 128) * 2;
            r = mix8(c1[0], c2[0], k);
            g = mix8(c1[1], c2[1], k);
            b = mix8(c1[2], c2[2], k);
        }
        if (amt) {
            r = mix8(r, tr, amt);
            g = mix8(g, tg, amt);
            b = mix8(b, tb, amt);
        }
        uint16_t c = c565(r, g, b);
        uint16_t *row = s_bg_buf + (size_t)y * WBG_W;
        for (int x = 0; x < WBG_W; x++) {
            row[x] = c;
        }
    }
    s_bg_key = slot * 8 + (s_wx.valid ? (s_wx.code / 10) : 7);
    if (s_bg_img) {
        lv_obj_invalidate(s_bg_img);
    }
}

/* ================= 小图标（用基本图元拼） ================= */

static lv_obj_t *shp(lv_obj_t *p, int x, int y, int w, int h,
                     uint32_t c, int opa, int radius)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, col(c), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

static void icon_sun(lv_obj_t *box, int s, int cx, int cy, uint32_t c)
{
    int d = s * 5 / 10;
    shp(box, cx - d / 2, cy - d / 2, d, d, c, 255, d / 2);
    int rl = s * 16 / 100;          /* 光芒长度 */
    int th = s * 7 / 100 + 1;       /* 光芒粗细 */
    int gap = d / 2 + s * 8 / 100;
    shp(box, cx - th / 2, cy - gap - rl, th, rl, c, 255, th / 2);
    shp(box, cx - th / 2, cy + gap, th, rl, c, 255, th / 2);
    shp(box, cx - gap - rl, cy - th / 2, rl, th, c, 255, th / 2);
    shp(box, cx + gap, cy - th / 2, rl, th, c, 255, th / 2);
}

static void icon_cloud(lv_obj_t *box, int s, int cx, int cy, uint32_t c, int opa)
{
    int d1 = s * 34 / 100, d2 = s * 46 / 100;
    shp(box, cx - s * 34 / 100, cy - s * 4 / 100, s * 68 / 100, s * 26 / 100, c, opa, s * 13 / 100);
    shp(box, cx - d2 / 2, cy - d2 * 3 / 5, d2, d2, c, opa, d2 / 2);
    shp(box, cx - s * 40 / 100, cy - d1 / 4, d1, d1, c, opa, d1 / 2);
    shp(box, cx + s * 6 / 100, cy - d1 / 2, d1, d1, c, opa, d1 / 2);
}

static void icon_draw(lv_obj_t *box, int size, int code)
{
    if (!box) {
        return;
    }
    lv_obj_clean(box);
    int s = size;
    int cx = s / 2, cy = s / 2;
    uint32_t sun = 0xFFD27A;
    uint32_t white = 0xF4F8FF;
    uint32_t gray = 0xC9D4E6;
    uint32_t rain = 0x9BD8FF;

    if (code == 0) {
        icon_sun(box, s, cx, cy, sun);
    } else if (code == 1) {
        icon_sun(box, s, cx - s * 12 / 100, cy - s * 12 / 100, sun);
        icon_cloud(box, s, cx + s * 10 / 100, cy + s * 12 / 100, white, 235);
    } else if (code == 2) {
        icon_cloud(box, s, cx, cy, white, 235);
    } else if (code == 3) {
        icon_cloud(box, s, cx, cy, gray, 235);
    } else if (code == 45 || code == 48) {
        for (int i = 0; i < 3; i++) {
            shp(box, s * 12 / 100, s * (30 + i * 18) / 100,
                s * 76 / 100, s * 8 / 100, gray, 220, s * 4 / 100);
        }
    } else if (code >= 71 && code <= 77) {
        icon_cloud(box, s, cx, cy - s * 8 / 100, white, 235);
        for (int i = 0; i < 3; i++) {
            int d = s * 10 / 100;
            shp(box, cx - s * 24 / 100 + i * s * 22 / 100, cy + s * 30 / 100,
                d, d, white, 240, d / 2);
        }
    } else if (code >= 85 && code <= 86) {
        icon_cloud(box, s, cx, cy - s * 8 / 100, white, 235);
        for (int i = 0; i < 3; i++) {
            int d = s * 10 / 100;
            shp(box, cx - s * 24 / 100 + i * s * 22 / 100, cy + s * 30 / 100,
                d, d, white, 240, d / 2);
        }
    } else if (code >= 95) {
        icon_cloud(box, s, cx, cy - s * 10 / 100, gray, 235);
        shp(box, cx - s * 4 / 100, cy + s * 18 / 100, s * 8 / 100, s * 18 / 100,
            0xFFD27A, 255, s * 3 / 100);
        shp(box, cx - s * 16 / 100, cy + s * 30 / 100, s * 16 / 100, s * 7 / 100,
            0xFFD27A, 255, s * 3 / 100);
    } else {
        /* 雨 / 阵雨 */
        icon_cloud(box, s, cx, cy - s * 8 / 100, white, 235);
        for (int i = 0; i < 3; i++) {
            shp(box, cx - s * 24 / 100 + i * s * 22 / 100, cy + s * 22 / 100,
                s * 6 / 100 + 1, s * 20 / 100, rain, 245, s * 3 / 100);
        }
    }
}

/* ================= UI 控件 ================= */

static lv_obj_t *s_root;
static lv_obj_t *s_lbl_city;
static lv_obj_t *s_lbl_updated;
static lv_obj_t *s_lbl_temp;
static lv_obj_t *s_neg;          /* 负号（96px 字体里没有 '-') */
static lv_obj_t *s_lbl_desc;
static lv_obj_t *s_lbl_feels;
static lv_obj_t *s_lbl_aqi;
static lv_obj_t *s_lbl_hour[5];
static lv_obj_t *s_ico_hour[5];
static lv_obj_t *s_tmp_hour[5];
static lv_obj_t *s_hour_now;         /* "现在"那张高亮卡的底板 */

static lv_obj_t *s_row_day[3];
static lv_obj_t *s_row_ico[3];
static lv_obj_t *s_row_desc[3];
static lv_obj_t *s_row_temp[3];
static lv_obj_t *s_bar_track[3];
static lv_obj_t *s_bar_fill[3];
static lv_obj_t *s_bar_lo[3];
static lv_obj_t *s_bar_hi[3];
static int s_bar_y[3];          /* 每行进度条的 y（相对卡片） */

static lv_obj_t *s_lbl_hum;
static lv_obj_t *s_lbl_wind;
static lv_obj_t *s_lbl_date;

/* ---- 进度条：低温→高温 渐变 + 两个圆点 ---- */
#define BAR_W 146

static void bar_apply(int i, float tmin, float tmax, float gmin, float gmax)
{
    float span = (gmax - gmin) < 0.1f ? 1.0f : (gmax - gmin);
    int usable = BAR_W - 6;
    int x0 = (int)((tmin - gmin) / span * usable);
    int x1 = (int)((tmax - gmin) / span * usable);
    if (x0 < 0) x0 = 0;
    if (x1 > usable) x1 = usable;
    if (x1 < x0 + 12) x1 = x0 + 12;
    if (x1 > usable) {
        x1 = usable;
        if (x0 > usable - 12) x0 = usable - 12;
    }

    if (s_bar_fill[i]) {
        lv_obj_set_pos(s_bar_fill[i], 168 + x0, s_bar_y[i]);
        lv_obj_set_size(s_bar_fill[i], x1 - x0, 4);
    }
    if (s_bar_lo[i]) {
        lv_obj_set_pos(s_bar_lo[i], 168 + x0 - 3, s_bar_y[i] - 2);
    }
    if (s_bar_hi[i]) {
        lv_obj_set_pos(s_bar_hi[i], 168 + x1 - 3, s_bar_y[i] - 2);
    }
}

static void apply_ui(void)
{
    if (!s_lbl_city) {
        return;
    }
    lv_label_set_text(s_lbl_city, s_cities[s_city_idx].name);

    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    static const char *wd[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    if (t.tm_year + 1900 >= 2026) {
        lv_label_set_text(s_lbl_date, wd[t.tm_wday]);
    }

    if (!s_wx.valid) {
        lv_label_set_text(s_lbl_temp, "--");
        lv_label_set_text(s_lbl_desc,
                          wifi_mgr_get_state() == WIFI_MGR_CONNECTED ? "获取中…" : "未连接");
        lv_label_set_text(s_lbl_feels, "体感 --");
        lv_label_set_text(s_lbl_aqi, "AQI --");
        lv_label_set_text(s_lbl_hum, "湿度 --");
        lv_label_set_text(s_lbl_wind, "风速 --");
        lv_label_set_text(s_lbl_updated, "");
        return;
    }

    char b[32];
    snprintf(b, sizeof(b), "%.0f", s_wx.temp);
    lv_label_set_text(s_lbl_temp, b);

    /* 负温度：96px 字体没有 '-'，用一根小横杠补上，并把数字右移 */
    if (s_wx.temp < -0.4f) {
        if (s_neg) {
            lv_obj_remove_flag(s_neg, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_pos(s_lbl_temp, 40, 66);
    } else {
        if (s_neg) {
            lv_obj_add_flag(s_neg, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_pos(s_lbl_temp, 12, 66);
    }
    lv_label_set_text(s_lbl_desc, wmo_text(s_wx.code));
    snprintf(b, sizeof(b), "%s 更新", s_wx.updated);
    lv_label_set_text(s_lbl_updated, b);

    snprintf(b, sizeof(b), "体感 %.0f°", s_wx.feels);
    lv_label_set_text(s_lbl_feels, b);
    /* 参考图里是 AQI + 等级；接口没有空气质量，这里用湿度替代会串味，
     * 所以按"湿度/AQI"两行分别显示真实数据 */
    if (s_wx.aqi > 0) {
        const char *lv = (s_wx.aqi <= 50) ? "优" : (s_wx.aqi <= 100) ? "良"
                        : (s_wx.aqi <= 150) ? "轻度" : (s_wx.aqi <= 200) ? "中度" : "重度";
        snprintf(b, sizeof(b), "AQI %d %s", s_wx.aqi, lv);
    } else {
        snprintf(b, sizeof(b), "AQI --");
    }
    lv_label_set_text(s_lbl_aqi, b);

    snprintf(b, sizeof(b), "湿度 %d%%", s_wx.humidity);
    lv_label_set_text(s_lbl_hum, b);
    snprintf(b, sizeof(b), "风速 %.0f km/h", s_wx.wind);
    lv_label_set_text(s_lbl_wind, b);

    /* 逐小时 */
    for (int i = 0; i < 5; i++) {
        if (!s_lbl_hour[i]) {
            continue;
        }
        if (i < s_wx.hourly_cnt) {
            if (i == 0) {
                lv_label_set_text(s_lbl_hour[i], "现在");
            } else {
                snprintf(b, sizeof(b), "%s时", s_wx.hourly[i].hh);
                lv_label_set_text(s_lbl_hour[i], b);
            }
            snprintf(b, sizeof(b), "%.0f°", s_wx.hourly[i].t);
            lv_label_set_text(s_tmp_hour[i], b);
            icon_draw(s_ico_hour[i], 26, s_wx.hourly[i].code);
        } else {
            lv_label_set_text(s_lbl_hour[i], "--");
            lv_label_set_text(s_tmp_hour[i], "--");
        }
    }

    /* 未来三天：先求 3 天总区间 */
    float gmin = 999, gmax = -999;
    for (int i = 0; i < s_wx.daily_cnt && i < 3; i++) {
        if (s_wx.daily[i].tmin < gmin) gmin = s_wx.daily[i].tmin;
        if (s_wx.daily[i].tmax > gmax) gmax = s_wx.daily[i].tmax;
    }
    for (int i = 0; i < 3; i++) {
        if (i < s_wx.daily_cnt) {
            lv_label_set_text(s_row_desc[i], wmo_text(s_wx.daily[i].code));
            icon_draw(s_row_ico[i], 22, s_wx.daily[i].code);
            snprintf(b, sizeof(b), "%.0f° / %.0f°", s_wx.daily[i].tmax, s_wx.daily[i].tmin);
            lv_label_set_text(s_row_temp[i], b);
            bar_apply(i, s_wx.daily[i].tmin, s_wx.daily[i].tmax, gmin, gmax);
        } else {
            lv_label_set_text(s_row_desc[i], "--");
            lv_label_set_text(s_row_temp[i], "-- / --");
        }
    }

    /* 背景：时段或天气变了才重画 */
    int key = day_slot(t.tm_hour) * 8 + (s_wx.valid ? (s_wx.code / 10) : 7);
    if (key != s_bg_key) {
        bg_render();
    }
}

static void city_next_cb(lv_event_t *e)
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
    s_bg_key = -1;
    apply_ui();
    ui_shell_toast(s_cities[s_city_idx].name);
    fetch_now();
}

static void city_prev_cb(lv_event_t *e)
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
    s_bg_key = -1;
    apply_ui();
    ui_shell_toast(s_cities[s_city_idx].name);
    fetch_now();
}

static void reset_ui_refs(void)
{
    s_root = NULL;
    s_bg_img = NULL;
    s_lbl_city = NULL;
    s_lbl_updated = NULL;
    s_lbl_temp = NULL;
    s_neg = NULL;
    s_lbl_desc = NULL;
    s_lbl_feels = NULL;
    s_lbl_aqi = NULL;
    memset(s_lbl_hour, 0, sizeof(s_lbl_hour));
    memset(s_ico_hour, 0, sizeof(s_ico_hour));
    memset(s_tmp_hour, 0, sizeof(s_tmp_hour));
    s_hour_now = NULL;
    memset(s_row_day, 0, sizeof(s_row_day));
    memset(s_row_ico, 0, sizeof(s_row_ico));
    memset(s_row_desc, 0, sizeof(s_row_desc));
    memset(s_row_temp, 0, sizeof(s_row_temp));
    memset(s_bar_track, 0, sizeof(s_bar_track));
    memset(s_bar_fill, 0, sizeof(s_bar_fill));
    memset(s_bar_lo, 0, sizeof(s_bar_lo));
    memset(s_bar_hi, 0, sizeof(s_bar_hi));
    s_lbl_hum = NULL;
    s_lbl_wind = NULL;
    s_lbl_date = NULL;
}

static void create(lv_obj_t *parent)
{
    reset_ui_refs();
    ui_app_topbar(parent, "天气");

    /* 先铺底色，图片没渲染出来也不会闪白 */
    lv_obj_set_style_bg_color(parent, col(0x0A1030), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    /* 根容器放 y=0：不会被 app_shift_up 上移，坐标就是屏幕坐标 */
    s_root = lv_obj_create(parent);
    lv_obj_set_size(s_root, 480, 480);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 背景画布 ---- */
    if (!s_bg_buf) {
        s_bg_buf = heap_caps_malloc(WBG_W * WBG_H * 2, MALLOC_CAP_SPIRAM);
        if (!s_bg_buf) {
            s_bg_buf = heap_caps_malloc(WBG_W * WBG_H * 2, MALLOC_CAP_DEFAULT);
        }
        ESP_LOGI(TAG, "bg canvas %p (%d KB)", s_bg_buf, WBG_W * WBG_H * 2 / 1024);
    }
    if (s_bg_buf) {
        memset(&s_bg_dsc, 0, sizeof(s_bg_dsc));
        s_bg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_bg_dsc.header.w = WBG_W;
        s_bg_dsc.header.h = WBG_H;
        s_bg_dsc.header.stride = WBG_W * 2;
        s_bg_dsc.data = (const uint8_t *)s_bg_buf;
        s_bg_dsc.data_size = WBG_W * WBG_H * 2;
    }
    s_bg_img = lv_image_create(s_root);
    lv_obj_set_pos(s_bg_img, 0, 0);
    if (s_bg_buf) {
        lv_image_set_src(s_bg_img, &s_bg_dsc);
    }
    lv_obj_add_flag(s_bg_img, LV_OBJ_FLAG_IGNORE_LAYOUT);

    /* ---- 城市 + 更新时间 ---- */
    s_lbl_city = ui_label(s_root, "深圳", &ui_cjk_24, C_TXT);
    lv_obj_set_pos(s_lbl_city, 16, 40);
    lv_obj_add_flag(s_lbl_city, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lbl_city, city_next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_lbl_city, city_prev_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* 定位小图标（城市右边的小圆点 + 小尾巴） */
    shp(s_root, 72, 50, 8, 8, 0xFFFFFF, 210, 4);
    shp(s_root, 75, 57, 2, 4, 0xFFFFFF, 210, 1);

    s_lbl_updated = ui_label(s_root, "", NULL, C_TXT);
    lv_obj_set_style_text_opa(s_lbl_updated, 170, 0);
    lv_obj_align(s_lbl_updated, LV_ALIGN_TOP_RIGHT, -16, 50);

    /* ---- 大温度 ---- */
    s_lbl_temp = lv_label_create(s_root);
    lv_label_set_text(s_lbl_temp, "--");
    lv_obj_set_style_text_font(s_lbl_temp, &ui_font_time96, 0);
    lv_obj_set_style_text_color(s_lbl_temp, col(C_TXT), 0);
    lv_obj_set_pos(s_lbl_temp, 12, 66);

    s_neg = shp(s_root, 12, 118, 26, 7, 0xFFFFFF, 255, 3);
    lv_obj_add_flag(s_neg, LV_OBJ_FLAG_HIDDEN);

    /* 度符号用一个小圆环画，凑近参考图那种"26°" */
    lv_obj_t *deg = shp(s_root, 12 + 118, 82, 16, 16, 0xFFFFFF, 0, 8);
    lv_obj_set_style_border_width(deg, 4, 0);
    lv_obj_set_style_border_color(deg, col(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(deg, 235, 0);

    /* ---- 右侧：天气 / 体感 / AQI ---- */
    s_lbl_desc = ui_label(s_root, "—", &ui_cjk_24, C_TXT);
    lv_obj_set_pos(s_lbl_desc, 214, 96);

    s_lbl_feels = ui_label(s_root, "体感 --", &ui_cjk_16, C_TXT);
    lv_obj_set_style_text_opa(s_lbl_feels, 190, 0);
    lv_obj_set_pos(s_lbl_feels, 214, 130);

    s_lbl_aqi = ui_label(s_root, "AQI --", &ui_cjk_16, C_TXT);
    lv_obj_set_style_text_opa(s_lbl_aqi, 190, 0);
    lv_obj_set_pos(s_lbl_aqi, 214, 154);

    /* ---- 逐小时 ---- */
    for (int i = 0; i < 5; i++) {
        int x = 16 + i * 90;
        if (i == 0) {
            s_hour_now = lv_obj_create(s_root);
            lv_obj_remove_flag(s_hour_now, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_size(s_hour_now, 80, 96);
            lv_obj_set_pos(s_hour_now, x - 4, 190);
            lv_obj_set_style_bg_color(s_hour_now, col(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(s_hour_now, 30, 0);
            lv_obj_set_style_border_width(s_hour_now, 1, 0);
            lv_obj_set_style_border_color(s_hour_now, col(0xFFFFFF), 0);
            lv_obj_set_style_border_opa(s_hour_now, 60, 0);
            lv_obj_set_style_radius(s_hour_now, 16, 0);
            lv_obj_set_style_pad_all(s_hour_now, 0, 0);
        }
        s_lbl_hour[i] = ui_label(s_root, "--", NULL, C_TXT);
        lv_obj_set_style_text_opa(s_lbl_hour[i], 200, 0);
        lv_obj_set_width(s_lbl_hour[i], 72);
        lv_obj_set_style_text_align(s_lbl_hour[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_lbl_hour[i], x, 198);

        s_ico_hour[i] = lv_obj_create(s_root);
        lv_obj_remove_flag(s_ico_hour[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_ico_hour[i], 26, 26);
        lv_obj_set_pos(s_ico_hour[i], x + 23, 222);
        lv_obj_set_style_bg_opa(s_ico_hour[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_ico_hour[i], 0, 0);
        lv_obj_set_style_pad_all(s_ico_hour[i], 0, 0);

        s_tmp_hour[i] = ui_label(s_root, "--", &lv_font_montserrat_20, C_TXT);
        lv_obj_set_width(s_tmp_hour[i], 72);
        lv_obj_set_style_text_align(s_tmp_hour[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_tmp_hour[i], x, 254);
    }

    /* ---- 未来三天 ---- */
    lv_obj_t *fc = lv_obj_create(s_root);
    lv_obj_remove_flag(fc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(fc, 448, 158);
    lv_obj_set_pos(fc, 16, 300);
    lv_obj_set_style_bg_color(fc, col(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(fc, 26, 0);
    lv_obj_set_style_border_width(fc, 1, 0);
    lv_obj_set_style_border_color(fc, col(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(fc, 45, 0);
    lv_obj_set_style_radius(fc, 20, 0);
    lv_obj_set_style_pad_all(fc, 0, 0);

    lv_obj_t *ft = ui_label(fc, "未来 3 天", NULL, C_TXT);
    lv_obj_set_style_text_opa(ft, 170, 0);
    lv_obj_set_pos(ft, 16, 10);

    static const char *dn[3] = { "明天", "后天", "大后天" };
    for (int i = 0; i < 3; i++) {
        int y = 40 + i * 38;
        s_row_day[i] = ui_label(fc, dn[i], NULL, C_TXT);
        lv_obj_set_pos(s_row_day[i], 16, y + 2);

        s_row_ico[i] = lv_obj_create(fc);
        lv_obj_remove_flag(s_row_ico[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_row_ico[i], 22, 22);
        lv_obj_set_pos(s_row_ico[i], 72, y);
        lv_obj_set_style_bg_opa(s_row_ico[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_row_ico[i], 0, 0);
        lv_obj_set_style_pad_all(s_row_ico[i], 0, 0);

        s_row_desc[i] = ui_label(fc, "--", NULL, C_TXT);
        lv_obj_set_style_text_opa(s_row_desc[i], 200, 0);
        lv_obj_set_pos(s_row_desc[i], 100, y + 2);

        s_bar_y[i] = y + 9;
        s_bar_track[i] = shp(fc, 168, s_bar_y[i], BAR_W, 4, 0xFFFFFF, 40, 2);
        s_bar_fill[i] = shp(fc, 168, s_bar_y[i], BAR_W, 4, 0x6FB6E8, 235, 2);
        lv_obj_set_style_bg_grad_color(s_bar_fill[i], col(0xF2A85C), 0);
        lv_obj_set_style_bg_grad_dir(s_bar_fill[i], LV_GRAD_DIR_HOR, 0);
        s_bar_lo[i] = shp(fc, 168, s_bar_y[i] - 2, 8, 8, 0x8FCBF0, 255, 4);
        s_bar_hi[i] = shp(fc, 168, s_bar_y[i] - 2, 8, 8, 0xFFD9A8, 255, 4);
        /* 圆点放到父级，免得被 fill 裁掉 */
        lv_obj_move_foreground(s_bar_lo[i]);
        lv_obj_move_foreground(s_bar_hi[i]);

        s_row_temp[i] = ui_label(fc, "-- / --", NULL, C_TXT);
        lv_obj_set_width(s_row_temp[i], 92);
        lv_obj_set_style_text_align(s_row_temp[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_row_temp[i], 338, y + 1);
    }

    /* ---- 底部 ---- */
    s_lbl_hum = ui_label(s_root, "湿度 --", NULL, C_TXT);
    lv_obj_set_style_text_opa(s_lbl_hum, 180, 0);
    lv_obj_set_pos(s_lbl_hum, 16, 462);

    s_lbl_wind = ui_label(s_root, "风速 --", NULL, C_TXT);
    lv_obj_set_style_text_opa(s_lbl_wind, 180, 0);
    lv_obj_align(s_lbl_wind, LV_ALIGN_TOP_MID, 0, 462);

    s_lbl_date = ui_label(s_root, "", NULL, C_TXT);
    lv_obj_set_style_text_opa(s_lbl_date, 180, 0);
    lv_obj_align(s_lbl_date, LV_ALIGN_TOP_RIGHT, -16, 462);

    weather_start();
    apply_ui();
}

/* ---- 供锁屏显示用的摘要（ui_shell 调用） ---- */
bool ui_weather_summary(float *temp, float *tmax, float *tmin, int *code, const char **city)
{
    if (!s_wx.valid) {
        return false;
    }
    if (temp) {
        *temp = s_wx.temp;
    }
    if (tmax) {
        *tmax = (s_wx.daily_cnt > 0) ? s_wx.daily[0].tmax : s_wx.temp;
    }
    if (tmin) {
        *tmin = (s_wx.daily_cnt > 0) ? s_wx.daily[0].tmin : s_wx.temp;
    }
    if (code) {
        *code = s_wx.code;
    }
    if (city) {
        *city = s_cities[s_city_idx].name;
    }
    return true;
}

const char *ui_weather_desc(void)
{
    return s_wx.valid ? wmo_text(s_wx.code) : "";
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
