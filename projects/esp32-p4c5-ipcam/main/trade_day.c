/*
 * 交易日历
 *
 * 价格提醒的"有效期"按交易日算（1 个交易日 / 30 个交易日），
 * 所以要能判断某天是不是交易日：周末 + 国家法定节假日 + 调休上班。
 *
 * 节假日从网络接口取（timor.tech 的免费节假日 API，返回里
 * holiday=true 表示放假、false 表示调休上班），取到后缓存进 NVS，
 * 断网也能用；没取到过就退回"只跳周末"。
 *
 * 接口形如：https://timor.tech/api/holiday/year/2026
 *   {"holiday":{"01-01":{"holiday":true,"name":"元旦",...},
 *               "01-04":{"holiday":false,...}}}
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "esp_heap_caps.h"

#include "trade_day.h"

static const char *TAG = "trade_day";

#define HOL_MAX     128

typedef struct {
    int32_t ymd;
    uint8_t off;        /* 1 = 休市（法定假日），0 = 调休上班 */
} hol_t;

/* 内置节假日（2025/2026，来自国务院放假安排；只存与"周一~周五上班"不同的日子）
 * 网络接口拉不到时用它兜底，保证有效期计算不会退回"只跳周末" */
static const hol_t s_builtin[] = {
    { 20250101, 1 }, { 20250126, 0 }, { 20250128, 1 }, { 20250129, 1 }, { 20250130, 1 }, { 20250131, 1 },
    { 20250203, 1 }, { 20250204, 1 }, { 20250208, 0 }, { 20250404, 1 }, { 20250427, 0 }, { 20250501, 1 },
    { 20250502, 1 }, { 20250505, 1 }, { 20250602, 1 }, { 20250928, 0 }, { 20251001, 1 }, { 20251002, 1 },
    { 20251003, 1 }, { 20251006, 1 }, { 20251007, 1 }, { 20251008, 1 }, { 20251011, 0 }, { 20260101, 1 },
    { 20260102, 1 }, { 20260104, 0 }, { 20260214, 0 }, { 20260216, 1 }, { 20260217, 1 }, { 20260218, 1 },
    { 20260219, 1 }, { 20260220, 1 }, { 20260223, 1 }, { 20260228, 0 }, { 20260406, 1 }, { 20260501, 1 },
    { 20260504, 1 }, { 20260505, 1 }, { 20260509, 0 }, { 20260619, 1 }, { 20260920, 0 }, { 20260925, 1 },
    { 20261001, 1 }, { 20261002, 1 }, { 20261005, 1 }, { 20261006, 1 }, { 20261007, 1 }, { 20261010, 0 },
};

static hol_t s_hol[HOL_MAX];
static int s_hol_cnt;
static volatile bool s_ready;      /* 是否已有日历（内置表也算） */
static volatile int  s_net_year;   /* 已从网络拿到哪一年，0 = 还没成功 */
static int s_fail;                 /* 连续失败次数（退避用） */
static volatile bool s_busy;
static volatile bool s_task_on;

/* ---- NVS 落盘（R-02）----
 * trade_task 的栈在 PSRAM，而写 flash 时会 assert(esp_task_stack_is_sane_cache_disabled)
 * —— PSRAM 栈必然不满足，一旦日历抓取成功就直接 panic 重启。
 * 所以这里只把要写的内容格式化好放进 s_save_buf，再唤醒一个**内部栈**的小任务去写。 */
static SemaphoreHandle_t s_save_sem;
static char s_save_buf[1600];
static volatile bool s_save_pending;

/* ================= 日期换算 ================= */

/* 公历 → 1970-01-01 起的天数（Howard Hinnant 算法，不依赖 libc 时区） */
static int ymd_to_days(int ymd)
{
    int y = ymd / 10000;
    int m = (ymd / 100) % 100;
    int d = ymd % 100;
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int)doe - 719468;
}

static int days_to_ymd(int z)
{
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1;
    unsigned m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    return y * 10000 + (int)m * 100 + (int)d;
}

int trade_day_today(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 < 2024) {
        return 0;               /* 时间还没同步 */
    }
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
}

/* ================= 表查询 ================= */

static int hol_find(int ymd)
{
    for (int i = 0; i < s_hol_cnt; i++) {
        if (s_hol[i].ymd == ymd) {
            return i;
        }
    }
    return -1;
}

static void hol_put(int ymd, bool off)
{
    int i = hol_find(ymd);
    if (i >= 0) {
        s_hol[i].off = off ? 1 : 0;
        return;
    }
    if (s_hol_cnt >= HOL_MAX) {
        return;
    }
    s_hol[s_hol_cnt].ymd = ymd;
    s_hol[s_hol_cnt].off = off ? 1 : 0;
    s_hol_cnt++;
}

bool trade_day_is_trading(int ymd)
{
    if (ymd < 20240101) {
        return false;
    }
    int e = hol_find(ymd);
    if (e >= 0) {
        return s_hol[e].off == 0;   /* 调休上班 → 交易；法定假日 → 休市 */
    }
    int wd = (ymd_to_days(ymd) + 4) % 7;    /* 0 = 周日 */
    if (wd < 0) {
        wd += 7;
    }
    return wd != 0 && wd != 6;
}

int trade_day_add(int ymd, int n)
{
    if (ymd <= 0) {
        return 0;
    }
    int d = ymd;
    int guard = 0;
    while (n > 0 && guard++ < 900) {
        d = days_to_ymd(ymd_to_days(d) + 1);
        if (trade_day_is_trading(d)) {
            n--;
        }
    }
    return d;
}

/* (from, to] 之间的交易日数量；to <= from 返回 0 */
int trade_day_count(int from_ymd, int to_ymd)
{
    if (from_ymd <= 0 || to_ymd <= 0 || to_ymd <= from_ymd) {
        return 0;
    }
    int a = ymd_to_days(from_ymd);
    int b = ymd_to_days(to_ymd);
    if (b - a > 800) {
        b = a + 800;
    }
    int cnt = 0;
    for (int i = a + 1; i <= b; i++) {
        if (trade_day_is_trading(days_to_ymd(i))) {
            cnt++;
        }
    }
    return cnt;
}

bool trade_day_ready(void)
{
    return s_ready;
}

/* ================= NVS 缓存 ================= */

/* 任意任务（含 PSRAM 栈）可调用：只格式化 + 置标志，不碰 flash */
static void hol_save(void)
{
    size_t o = 0;
    s_save_buf[0] = '\0';
    for (int i = 0; i < s_hol_cnt && o + 14 < sizeof(s_save_buf); i++) {
        o += (size_t)snprintf(s_save_buf + o, sizeof(s_save_buf) - o, "%s%d%c",
                              i ? "," : "", (int)s_hol[i].ymd,
                              s_hol[i].off ? 'R' : 'W');
    }
    s_save_pending = true;
    if (s_save_sem) {
        xSemaphoreGive(s_save_sem);
    }
}

/* 内部栈任务：唯一允许写 flash 的地方 */
static void nvs_writer_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_save_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!s_save_pending) {
            continue;
        }
        s_save_pending = false;
        nvs_handle_t h;
        esp_err_t err = nvs_open("trade", NVS_READWRITE, &h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "nvs open failed: %s", esp_err_to_name(err));
            continue;
        }
        err = nvs_set_str(h, "hol", s_save_buf);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "holiday cache saved (%u B)", (unsigned)strlen(s_save_buf));
        } else {
            ESP_LOGW(TAG, "holiday cache save failed: %s", esp_err_to_name(err));
        }
    }
}

static void hol_load(void)
{
    char buf[1600] = { 0 };
    size_t len = sizeof(buf);
    nvs_handle_t h;
    if (nvs_open("trade", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    esp_err_t err = nvs_get_str(h, "hol", buf, &len);
    nvs_close(h);
    if (err != ESP_OK || !buf[0]) {
        return;
    }
    s_hol_cnt = 0;
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t && s_hol_cnt < HOL_MAX;
         t = strtok_r(NULL, ",", &save)) {
        size_t n = strlen(t);
        if (n < 9) {
            continue;
        }
        char c = t[n - 1];
        t[n - 1] = '\0';
        int ymd = atoi(t);
        if (ymd > 20200101) {
            hol_put(ymd, c == 'R');
        }
    }
    s_ready = s_hol_cnt > 0;
    ESP_LOGI(TAG, "holiday cache loaded: %d day(s)", s_hol_cnt);
}

/* ================= 网络抓取 ================= */

/* 解析 apihubs 的日列表：{"date":20260101,...,"workday":1}
 * workday=1 上班（= 交易日），2 休息 */
static int parse_holidays(const char *body, int year)
{
    int added = 0;
    const char *p = body;
    while ((p = strstr(p, "\"date\":")) != NULL) {
        const char *d = p + 7;
        while (*d == ' ') {
            d++;
        }
        int ymd = 0;
        int nd = 0;
        while (nd < 8 && d[nd] >= '0' && d[nd] <= '9') {
            ymd = ymd * 10 + (d[nd] - '0');
            nd++;
        }
        if (nd != 8 || ymd / 10000 != year) {
            p = d;
            continue;
        }
        const char *w = strstr(d, "\"workday\":");
        if (!w) {
            break;
        }
        /* 只认同一条目内的字段（下一条 date 之前） */
        const char *next = strstr(d, "\"date\":");
        if (next && next < w) {
            p = w;
            continue;
        }
        const char *v = w + 10;
        while (*v == ' ') {
            v++;
        }
        int wd = (ymd_to_days(ymd) + 4) % 7;     /* 0 = 周日 */
        if (wd < 0) {
            wd += 7;
        }
        bool weekend = (wd == 0 || wd == 6);
        if (*v == '1' && weekend) {             /* 周末调休上班 */
            hol_put(ymd, false);
            added++;
        } else if (*v == '2' && !weekend) {     /* 工作日放假 */
            hol_put(ymd, true);
            added++;
        }
        p = v;
    }
    return added;
}

static bool fetch_year(int year)
{
    char url[96];
    /* 国内可直连的免费接口（返回整年，size=400 一次拿完） */
    snprintf(url, sizeof(url), "https://api.apihubs.cn/holiday/get?year=%d&size=400", year);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 2048,
        /* Cloudflare 会拦太短的 UA，用完整 Chrome UA */
        .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                      "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36",
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return false;
    }
    bool ok = false;
    char *body = NULL;
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        int status = esp_http_client_get_status_code(cli);
        /* 整年约 110KB，必须放 PSRAM；用 8KB 栈缓冲会被截断，
         * 只剩最后几天的数据（而且会误清空已知数据） */
        const size_t cap = 192 * 1024;
        body = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
        int total = 0;
        if (body) {
            while (total < (int)cap - 1) {
                int r = esp_http_client_read(cli, body + total, cap - 1 - total);
                if (r <= 0) {
                    break;
                }
                total += r;
            }
            body[total] = '\0';
        }
        int n = 0;
        if (body && total > 0 && status == 200) {
            n = parse_holidays(body, year);
        }
        if (n == 0 && body && strstr(body, "80006")) {
            ESP_LOGW(TAG, "holiday %d: rate limited, retry later", year);
        } else {
            ESP_LOGI(TAG, "holiday %d: http=%d bytes=%d entries=%d", year, status, total, n);
        }
        ok = n > 0;
    } else {
        ESP_LOGW(TAG, "holiday %d: http open failed (%s)", year, esp_err_to_name(err));
    }
    if (body) {
        heap_caps_free(body);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return ok;
}

static void fetch_all(void)
{
    int today = trade_day_today();
    if (today <= 0) {
        return;
    }
    int year = today / 10000;
    bool any = fetch_year(year);
    vTaskDelay(pdMS_TO_TICKS(3000));      /* 接口限流：两次请求之间要停一下 */
    if (fetch_year(year + 1)) {
        any = true;
    }
    if (any) {
        s_ready = true;
        s_net_year = year;
        hol_save();
        ESP_LOGI(TAG, "calendar ready, %d day(s) cached", s_hol_cnt);
    }
}

static void trade_task(void *arg)
{
    (void)arg;
    /* 等时间同步（开机 WiFi + SNTP 要二十秒左右） */
    for (int i = 0; i < 300 && trade_day_today() <= 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    /* 还没从网络拿到今年的日历就每 3 分钟重试一次（开机时 DNS 可能还没就绪），
     * 拿到之后 6 小时刷一次 */
    while (1) {
        int y = trade_day_today() / 10000;
        bool need = (y <= 0) || (s_net_year != y);
        if (y > 0 && s_net_year != y) {
            fetch_all();
        }
        if (!need) {
            s_fail = 0;
            vTaskDelay(pdMS_TO_TICKS(6 * 3600 * 1000));
            continue;
        }
        /* 失败就退避：3 → 6 → 12 → 24 → 48 → 96 分钟，最多 4 小时，
         * 别把有限的接口配额（还限流）打光 */
        int wait_min = 3 << (s_fail > 5 ? 5 : s_fail);
        if (wait_min > 240) {
            wait_min = 240;
        }
        if (s_fail < 8) {
            s_fail++;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_min * 60 * 1000));
    }
}

void trade_day_refresh(void)
{
    /* 由任务自己周期拉取，这里只保证任务在跑 */
    trade_day_init();
}

void trade_day_init(void)
{
    if (s_task_on) {
        return;
    }
    s_task_on = true;
    /* 先读 NVS 里上次抓到的（含网络更新），再用内置表补齐缺的：
     * 这样断网也能算交易日，不会退回"只跳周末" */
    hol_load();
    for (size_t i = 0; i < sizeof(s_builtin) / sizeof(s_builtin[0]); i++) {
        if (hol_find(s_builtin[i].ymd) < 0) {
            hol_put(s_builtin[i].ymd, s_builtin[i].off != 0);
        }
    }
    s_ready = true;
    /* 落盘任务：内部栈（写 flash 必须），栈给 3072 够用 */
    if (!s_save_sem) {
        s_save_sem = xSemaphoreCreateBinary();
        if (s_save_sem) {
            if (xTaskCreate(nvs_writer_task, "trade_nvs", 3072, NULL, 1, NULL) != pdPASS) {
                ESP_LOGE(TAG, "create trade_nvs failed");
            }
        }
    }
    /* 走 HTTPS，栈给足并放 PSRAM（TLS 握手很吃栈） */
    xTaskCreateWithCaps(trade_task, "trade_day", 10240, NULL, 2, NULL, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "calendar task started (cached=%d, ready=%d)", s_hol_cnt, s_ready);
}
