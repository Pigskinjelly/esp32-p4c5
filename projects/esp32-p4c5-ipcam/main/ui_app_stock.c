/*
 * 股票 App —— 左栏大盘指数 + 右栏自选股
 *
 * 数据源：腾讯行情接口（稳定、免鉴权）
 *   https://qt.gtimg.cn/q=sh000001,sz399001,...,sh600519,...
 *   字段以 ~ 分隔（接口里有空字段，必须保留空串，不能用 strtok）：
 *     [1]=名称(GBK) [3]=现价 [4]=昨收 [31]=涨跌额 [32]=涨跌% [33]=最高 [34]=最低
 *
 * 自选股存 NVS（"stock"/"list"，格式 secid|名称;...），最多 8 只。
 */
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lvgl.h"
#include "ui_shell.h"
#include "ui_theme.h"
#include "wifi_mgr.h"
#include "gbk2utf8.h"
#include "trade_day.h"
#include "stock_cfg.h"

static const char *TAG = "ui_stock";

#define MAX_WATCH   8
#define MAX_IDX     6      /* 左栏最多 6 个指数（可滚动） */

/* 市场：A 股 / 美股。版式完全一样，只是数据源、时区、默认指数不同 */
typedef enum { MKT_A = 0, MKT_US = 1 } mkt_t;
static mkt_t s_mkt = MKT_A;

/* ================= 数据 ================= */

typedef struct {
    char  secid[12];
    char  code[8];
    char  name[32];
    float price;
    float chg;
    float pct;
    float high;
    float low;
    float open;
    float prev;
    float vol;          /* 成交量（手） */
    float amt;          /* 成交额（万元） */
    int   dec;          /* 价格小数位：股票 2、ETF/LOF 3 */
    char  ksym[16];     /* K 线代码：A 股同 secid；美股用 "usAAPL.OQ" / "us.DJI" */
    bool  valid;
} quote_t;

/* 默认指数（第一次启动时写入；之后由用户增删，顺序也由用户拖动决定） */
static const char *A_IDX_DEF[][2] = {
    { "sh000001", "上证" }, { "sz399001", "深证" },
    { "sz399006", "创业" }, { "sh000688", "科创" },
};
static const char *US_IDX_DEF[][2] = {
    { "usDJI", "道琼斯" }, { "usNDX", "纳指100" }, { "usINX", "标普500" },
};

static quote_t s_idx[MAX_IDX];      /* 左栏：指数 */
static int s_idx_cnt;
static quote_t s_watch[MAX_WATCH];  /* 右栏：个股 */
static int s_watch_cnt;

static volatile bool s_dirty;
static volatile bool s_busy;
static SemaphoreHandle_t s_sem;

/* 前置声明（这些函数在后面定义） */
static volatile bool s_visible;
static volatile bool s_alert_ui_dirty;   /* 提醒变动，等 tick 里刷 UI */
static void alert_check(void);
static void alert_drop_stock(const char *secid);
static void alert_load(void);
static void row_click_cb(lv_event_t *e);
static void row_long_cb(lv_event_t *e);
static void row_pressing_cb(lv_event_t *e);
static void row_release_cb(lv_event_t *e);
static void mkt_switch_cb(lv_event_t *e);
static void kperiod_cb(lv_event_t *e);
static void mkt_load(void);
static void rebuild_idx(void);
static void rebuild_list(void);
static void open_idx_detail(int idx);
static lv_obj_t *s_mkt_btn[2];
static void fetch_now(void);
static void open_detail(int idx);
static void detail_refresh(void);
static void alist_rebuild(void);
static void alist_open(const char *secid);
static void alist_price_refresh(void);
static lv_obj_t *s_alist;

/* ================= 交易状态（A 股 / 美股） ================= */

/*
 * A 股（北京时间）：
 * 09:15-09:30 竞价 / 09:30-11:30 交易中 / 11:30-13:00 午休
 * 13:00-14:57 交易中 / 14:57-15:00 竞价 / 15:00-15:30 盘后
 * 美股（美东时间）：
 * 04:00-09:30 盘前 / 09:30-16:00 交易中 / 16:00-20:00 盘后
 */
static const char *trade_status_a(struct tm *t, uint32_t *color)
{
    if (t->tm_wday == 0 || t->tm_wday == 6) {
        *color = 0x8FA8CC;
        return "已收盘";
    }
    int m = t->tm_hour * 60 + t->tm_min;
    if (m >= 9 * 60 + 15 && m < 9 * 60 + 30)      { *color = 0xE8C537; return "竞价"; }
    if (m >= 9 * 60 + 30 && m < 11 * 60 + 30)     { *color = 0x37E8A0; return "交易中"; }
    if (m >= 11 * 60 + 30 && m < 13 * 60)         { *color = 0xE8A537; return "午休"; }
    if (m >= 13 * 60 && m < 14 * 60 + 57)         { *color = 0x37E8A0; return "交易中"; }
    if (m >= 14 * 60 + 57 && m < 15 * 60)         { *color = 0xE8C537; return "竞价"; }
    if (m >= 15 * 60 && m < 15 * 60 + 30)         { *color = 0xB08CE8; return "盘后"; }
    *color = 0x8FA8CC;
    return "已收盘";
}

static const char *trade_status_us(struct tm *t, uint32_t *color)
{
    if (t->tm_wday == 0 || t->tm_wday == 6) {
        *color = 0x8FA8CC;
        return "休市";
    }
    int m = t->tm_hour * 60 + t->tm_min;
    if (m >= 9 * 60 + 30 && m < 16 * 60)        { *color = 0x37E8A0; return "交易中"; }
    if (m >= 4 * 60 && m < 9 * 60 + 30)         { *color = 0xE8C537; return "盘前"; }
    if (m >= 16 * 60 && m < 20 * 60)            { *color = 0xB08CE8; return "盘后"; }
    *color = 0x8FA8CC;
    return "休市";
}

/* 某年某月 1 号是星期几（0=周日）—— Zeller 同余，不依赖 timegm */
static int wday_of(int year, int mon)
{
    int y = year, m = mon;
    if (m < 3) { m += 12; y -= 1; }
    int k = y % 100, j = y / 100;
    int h = (1 + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return (h + 6) % 7;     /* Zeller: 0=周六 → 换成 0=周日 */
}

/* 美东夏令时：3 月第 2 个周日 ~ 11 月第 1 个周日，UTC-4；其余 UTC-5 */
static int us_utc_off(int year, int mon, int day)
{
    int mar1 = wday_of(year, 3);
    int dst_start = 8 + ((7 - mar1) % 7);        /* 3 月第 2 个周日 */
    int nov1 = wday_of(year, 11);
    int dst_end = 1 + ((7 - nov1) % 7);          /* 11 月第 1 个周日 */
    if (mon > 3 && mon < 11) {
        return -4;
    }
    if (mon == 3) {
        return (day >= dst_start) ? -4 : -5;
    }
    if (mon == 11) {
        return (day < dst_end) ? -4 : -5;
    }
    return -5;
}

/* 把 UTC 换成美东时间 */
static void to_us_east(time_t utc, struct tm *out)
{
    struct tm u;
    gmtime_r(&utc, &u);
    int off = us_utc_off(u.tm_year + 1900, u.tm_mon + 1, u.tm_mday);
    time_t et = utc + (time_t)(off * 3600);
    gmtime_r(&et, out);
}

static const char *trade_status(struct tm *local, uint32_t *color)
{
    if (s_mkt == MKT_US) {
        struct tm et;
        to_us_east(time(NULL), &et);
        return trade_status_us(&et, color);
    }
    return trade_status_a(local, color);
}

/* ================= 列表持久化 =================
 * 四个列表各存一份（NVS 命名空间 stock）：
 *   alist= A 股个股   ulist= 美股个股
 *   aidx = A 股指数   uidx = 美股指数
 * 格式 "secid|name;secid|name"，顺序就是用户拖出来的顺序。
 */

static const char *A_WATCH_DEF[][2] = {
    { "sh600519", "贵州茅台" }, { "sz000001", "平安银行" },
    { "sz002517", "恺英网络" },
};
static const char *US_WATCH_DEF[][2] = {
    { "usAAPL", "苹果" }, { "usNVDA", "英伟达" },
    { "usTSLA", "特斯拉" }, { "usMSFT", "微软" },
};

static void list_save(const char *key, const quote_t *arr, int cnt)
{
    char buf[512] = { 0 };
    size_t off = 0;
    for (int i = 0; i < cnt && off < sizeof(buf) - 48; i++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s%s|%s",
                                i ? ";" : "", arr[i].secid, arr[i].name);
    }
    nvs_handle_t h;
    if (nvs_open("stock", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, key, buf);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "%s saved: %s", key, buf);
}

/* 读一个列表；没有就写默认值。def 可以为 NULL（表示允许空列表） */
static int list_load(const char *key, quote_t *arr, int max,
                     const char *def[][2], int defn)
{
    char buf[512] = { 0 };
    size_t len = sizeof(buf);
    nvs_handle_t h;
    if (nvs_open("stock", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_str(h, key, buf, &len) != ESP_OK) {
            buf[0] = '\0';
        }
        nvs_close(h);
    }
    int cnt = 0;
    if (buf[0]) {
        char *save = NULL;
        char *tok = strtok_r(buf, ";", &save);
        while (tok && cnt < max) {
            char *bar = strchr(tok, '|');
            quote_t *w = &arr[cnt];
            memset(w, 0, sizeof(*w));
            if (bar) {
                *bar = '\0';
                strlcpy(w->name, bar + 1, sizeof(w->name));
            }
            strlcpy(w->secid, tok, sizeof(w->secid));
            /* 兼容早期东财格式 "0.002517" → "sz002517" */
            if (strchr(w->secid, '.')) {
                char tmp[12];
                snprintf(tmp, sizeof(tmp), "%s%s",
                         w->secid[0] == '1' ? "sh" : "sz", w->secid + 2);
                strlcpy(w->secid, tmp, sizeof(w->secid));
            }
            strlcpy(w->code, w->secid + 2, sizeof(w->code));
            strlcpy(w->ksym, w->secid, sizeof(w->ksym));
            cnt++;
            tok = strtok_r(NULL, ";", &save);
        }
    }
    if (cnt == 0 && def) {
        for (int i = 0; i < defn && i < max; i++) {
            memset(&arr[i], 0, sizeof(arr[i]));
            strlcpy(arr[i].secid, def[i][0], sizeof(arr[i].secid));
            strlcpy(arr[i].name, def[i][1], sizeof(arr[i].name));
            strlcpy(arr[i].code, def[i][0] + 2, sizeof(arr[i].code));
            strlcpy(arr[i].ksym, def[i][0], sizeof(arr[i].ksym));
        }
        cnt = defn;
        list_save(key, arr, cnt);
    }
    return cnt;
}

static void watch_save(void)
{
    list_save(s_mkt == MKT_A ? "alist" : "ulist", s_watch, s_watch_cnt);
}

static void watch_load(void)
{
    memset(s_watch, 0, sizeof(s_watch));
    if (s_mkt == MKT_A) {
        /* 老固件只存过 "list"，先迁移过来 */
        s_watch_cnt = list_load("alist", s_watch, MAX_WATCH, NULL, 0);
        if (s_watch_cnt == 0) {
            s_watch_cnt = list_load("list", s_watch, MAX_WATCH, A_WATCH_DEF, 3);
            if (s_watch_cnt == 3) {
                list_save("alist", s_watch, s_watch_cnt);
            }
        }
    } else {
        s_watch_cnt = list_load("ulist", s_watch, MAX_WATCH, US_WATCH_DEF, 4);
    }
    ESP_LOGI(TAG, "watchlist(%s): %d", s_mkt == MKT_A ? "A" : "US", s_watch_cnt);
}

static void idx_save(void)
{
    list_save(s_mkt == MKT_A ? "aidx" : "uidx", s_idx, s_idx_cnt);
}

static void idx_load(void)
{
    memset(s_idx, 0, sizeof(s_idx));

    /* v2 迁移：美股指数默认改成 道琼斯 / 纳指100 / 标普500
     * （旧版留了 IXIC，会和 NDX 重复），顺手把跑测试时累计的启动计数清零 */
    {
        nvs_handle_t h;
        if (nvs_open("stock", NVS_READWRITE, &h) == ESP_OK) {
            uint8_t ver = 0;
            if (nvs_get_u8(h, "idxver", &ver) != ESP_OK || ver < 2) {
                nvs_erase_key(h, "uidx");
                nvs_set_u8(h, "idxver", 2);
                nvs_commit(h);
                ESP_LOGW(TAG, "migrate: reset US index list to v2 defaults");
            }
            nvs_close(h);
        }
    }

    if (s_mkt == MKT_A) {
        s_idx_cnt = list_load("aidx", s_idx, MAX_IDX, A_IDX_DEF, 4);
    } else {
        s_idx_cnt = list_load("uidx", s_idx, MAX_IDX, US_IDX_DEF, 3);
    }
    ESP_LOGI(TAG, "index(%s): %d", s_mkt == MKT_A ? "A" : "US", s_idx_cnt);
}

static void code_to_secid(const char *code, char *secid, size_t sz)
{
    if (s_mkt == MKT_US) {
        char tmp[10];
        const char *c = code;
        if (strncasecmp(c, "us", 2) == 0) {
            c += 2;
        }
        snprintf(tmp, sizeof(tmp), "%.9s", c);
        for (char *p = tmp; *p; p++) {
            *p = (char)toupper((unsigned char)*p);
        }
        snprintf(secid, sz, "us%.9s", tmp);
        return;
    }
    const char *mkt = (code[0] == '6' || code[0] == '9' || code[0] == '5') ? "sh" : "sz";
    snprintf(secid, sz, "%s%s", mkt, code);
}

/* 价格小数位：接口给的是字符串（如 "1.234"），按原文里的小数位显示。
 * A 股 2 位、ETF/LOF/基金 3 位。之前统一 "%.2f" 会把 ETF 的第三位抹掉。 */
static int decimals_of(const char *s)
{
    const char *dot = s ? strchr(s, '.') : NULL;
    if (!dot) {
        return 2;
    }
    int n = 0;
    while (dot[1 + n] >= '0' && dot[1 + n] <= '9') {
        n++;
    }
    if (n < 2) {
        n = 2;
    }
    if (n > 4) {
        n = 4;
    }
    return n;
}

/* 按位数格式化价格（lv_label_set_text_fmt 不支持 "%.*f"，先用 stdio 拼好） */
static void fmt_price(char *buf, size_t cap, float v, int dec)
{
    snprintf(buf, cap, "%.*f", (dec >= 2 && dec <= 4) ? dec : 2, v);
}

/* ================= 行情抓取 ================= */

/** 按 '~' 切分但**保留空字段**（接口第 29 项是空串，strtok 会吞掉导致索引错位） */
static int split_tilde(char *s, char **out, int max)
{
    int n = 0;
    out[n++] = s;
    for (char *p = s; *p && n < max; p++) {
        if (*p == '~') {
            *p = '\0';
            out[n++] = p + 1;
        }
    }
    return n;
}

static void parse_one_line(char *seg)
{
    char *eq = strchr(seg, '=');
    if (!eq || eq - seg < 3) {
        return;
    }
    char secid[12];
    size_t n = (size_t)(eq - (seg + 2));
    if (n == 0 || n >= sizeof(secid)) {
        return;
    }
    memcpy(secid, seg + 2, n);
    secid[n] = '\0';

    char *p = eq + 1;
    if (*p == '"') {
        p++;
    }
    char *f[48];
    int nf = split_tilde(p, f, 48);
    if (nf < 35) {
        ESP_LOGW(TAG, "field count %d too few for %s", nf, secid);
        return;
    }

    /* 美股 K 线要用接口回的第 3 栏（如 AAPL.OQ / .DJI），加 us 前缀 */
    char ksym[16];
    if (strncmp(secid, "us", 2) == 0) {
        if (f[2] && f[2][0]) {
            snprintf(ksym, sizeof(ksym), "us%s", f[2]);
        } else {
            strlcpy(ksym, secid, sizeof(ksym));
        }
    } else {
        strlcpy(ksym, secid, sizeof(ksym));
    }

    /* 指数（左栏，可增删） */
    for (int i = 0; i < s_idx_cnt; i++) {
        if (strcmp(s_idx[i].secid, secid) != 0) {
            continue;
        }
        quote_t *q = &s_idx[i];
        if (f[1] && f[1][0] && strcmp(secid, "us") != 0) {
            char name[64];
            gbk_to_utf8(f[1], name, sizeof(name));
            if (name[0] && strcmp(name, q->name) != 0) {
                strlcpy(q->name, name, sizeof(q->name));
                idx_save();
            }
        }
        q->price = (float)atof(f[3]);
        q->chg = (float)atof(f[31]);
        q->pct = (float)atof(f[32]);
        q->high = (float)atof(f[33]);
        q->low = (float)atof(f[34]);
        q->open = (float)atof(f[5]);
        q->prev = (float)atof(f[4]);
        q->vol = (float)atof(f[6]);
        q->amt = (float)atof(f[37]);
        q->dec = decimals_of(f[3]);
        strlcpy(q->ksym, ksym, sizeof(q->ksym));
        q->valid = true;
        return;
    }

    /* 自选股 */
    for (int k = 0; k < s_watch_cnt; k++) {
        if (strcmp(s_watch[k].secid, secid) != 0) {
            continue;
        }
        quote_t *w = &s_watch[k];
        if (f[1] && f[1][0]) {
            char name[64];
            gbk_to_utf8(f[1], name, sizeof(name));
            if (name[0] && strcmp(name, w->name) != 0) {
                strlcpy(w->name, name, sizeof(w->name));
                watch_save();
            }
        }
        w->price = (float)atof(f[3]);
        w->chg = (float)atof(f[31]);
        w->pct = (float)atof(f[32]);
        w->high = (float)atof(f[33]);
        w->low = (float)atof(f[34]);
        w->open = (float)atof(f[5]);
        w->prev = (float)atof(f[4]);
        w->vol = (float)atof(f[6]);
        w->amt = (float)atof(f[37]);
        w->dec = decimals_of(f[3]);      /* ETF 3 位、股票 2 位 */
        strlcpy(w->ksym, ksym, sizeof(w->ksym));
        /* 接口偶尔只给 "1.23"：基金/ETF 代码再兜一层，保证 3 位 */
        if (w->dec == 2 && s_watch[k].code[0] == '5') {
            w->dec = 3;
        }
        if (w->dec == 2 && s_watch[k].code[0] == '1' &&
            (s_watch[k].code[1] == '5' || s_watch[k].code[1] == '6')) {
            w->dec = 3;
        }
        w->valid = true;
        return;
    }
}

static void parse_response(char *body)
{
    char *save = NULL;
    char *line = strtok_r(body, ";\r\n", &save);
    int cnt = 0;
    while (line) {
        if (line[0] == 'v' && line[1] == '_') {
            parse_one_line(line);
            cnt++;
        }
        line = strtok_r(NULL, ";\r\n", &save);
    }
    s_dirty = true;
    ESP_LOGI(TAG, "quotes updated, %d item(s)", cnt);
}

static void fetch_task(void *arg)
{
    (void)arg;
    while (1) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (wifi_mgr_get_state() != WIFI_MGR_CONNECTED) {
            s_busy = false;
            continue;
        }
        char url[512];
        size_t off = (size_t)snprintf(url, sizeof(url), "http://qt.gtimg.cn/q=");
        for (int i = 0; i < s_idx_cnt && off < sizeof(url) - 16; i++) {
            off += (size_t)snprintf(url + off, sizeof(url) - off, "%s,", s_idx[i].secid);
        }
        for (int i = 0; i < s_watch_cnt && off < sizeof(url) - 16; i++) {
            off += (size_t)snprintf(url + off, sizeof(url) - off, "%s%s",
                                    i ? "," : "", s_watch[i].secid);
        }
        if (off >= sizeof(url)) {       /* snprintf 截断时 off 可能超出，兜一下 */
            off = sizeof(url) - 1;
        }

        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 8000,
            .buffer_size = 2048,
            .user_agent = "Mozilla/5.0 (ESP32-P4C5)",
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            esp_err_t err = esp_http_client_open(client, 0);
            if (err == ESP_OK) {
                esp_http_client_fetch_headers(client);
                int status = esp_http_client_get_status_code(client);
                static char body[7168];
                int total = 0;
                while (total < (int)sizeof(body) - 1) {
                    int r = esp_http_client_read(client, body + total, sizeof(body) - 1 - total);
                    if (r <= 0) break;
                    total += r;
                }
                body[total] = '\0';
                ESP_LOGI(TAG, "quote http=%d bytes=%d", status, total);
                if (total > 0 && status == 200) {
                    parse_response(body);
                    alert_check();
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

/* ================= UI ================= */

static lv_obj_t *s_list;
static lv_obj_t *s_lbl_empty;
static lv_obj_t *s_lbl_count;
static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_clock;
static lv_obj_t *s_lbl_date;

/* 左栏指数（数量可变，卡片重建，指针放这里） */
typedef struct {
    lv_obj_t *card;
    lv_obj_t *name;
    lv_obj_t *val;
    lv_obj_t *pct;
} idx_ui_t;
static idx_ui_t s_idxui[MAX_IDX];
static lv_obj_t *s_idx_list;        /* 左栏滚动容器 */

/* 右栏自选行 */
typedef struct {
    lv_obj_t *price;
    lv_obj_t *chg;
    lv_obj_t *hl;
    lv_obj_t *name;
} row_ui_t;
static row_ui_t s_rows[MAX_WATCH];

static lv_obj_t *s_add_modal;
static lv_obj_t *s_add_ta;

static uint32_t pct_color(float pct)
{
    return (pct > 0.0001f) ? C_UP : (pct < -0.0001f) ? C_DOWN : C_TXT_DIM;
}

static int find_index_by_secid(const char *secid)
{
    for (int i = 0; i < s_watch_cnt; i++) {
        if (strcmp(s_watch[i].secid, secid) == 0) {
            return i;
        }
    }
    return -1;
}

/* ================= 长按拖动排序（指数栏 / 个股栏共用） ================= */
static int  s_drag_kind;        /* 0=没在拖 1=指数 2=个股 */
static int  s_drag_from;
static int  s_drag_step;
static int  s_drag_y0;
static lv_coord_t s_drag_py0;
static lv_obj_t *s_drag_obj;
static int64_t s_drag_end_ms;   /* 刚拖完，屏蔽随后那次 CLICKED */

static void reorder_list(quote_t *arr, int cnt, int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= cnt || to >= cnt) {
        return;
    }
    quote_t tmp = arr[from];
    if (to > from) {
        memmove(&arr[from], &arr[from + 1], sizeof(quote_t) * (size_t)(to - from));
    } else {
        memmove(&arr[to + 1], &arr[to], sizeof(quote_t) * (size_t)(from - to));
    }
    arr[to] = tmp;
}

static void drag_begin(lv_obj_t *card, int kind, int from, int step)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (!indev) {
        return;
    }
    lv_indev_get_point(indev, &p);
    s_drag_kind = kind;
    s_drag_from = from;
    s_drag_step = step;
    s_drag_y0 = lv_obj_get_y(card);
    s_drag_py0 = p.y;
    s_drag_obj = card;
    lv_obj_t *list = lv_obj_get_parent(card);
    if (list) {
        lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);   /* 拖动时别滚 */
    }
    lv_obj_set_style_border_color(card, col(0x8B5CF6), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_move_foreground(card);
    ESP_LOGI(TAG, "drag begin kind=%d from=%d", kind, from);
}

static void drag_move(lv_obj_t *card)
{
    if (!s_drag_obj || s_drag_obj != card) {
        return;
    }
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (!indev) {
        return;
    }
    lv_indev_get_point(indev, &p);
    lv_obj_set_y(card, s_drag_y0 + (p.y - s_drag_py0));
}

static void drag_end(lv_obj_t *card)
{
    if (!s_drag_obj || s_drag_obj != card) {
        return;
    }
    int dy = lv_obj_get_y(card) - s_drag_y0;
    int shift = (dy >= 0) ? (dy + s_drag_step / 2) / s_drag_step
                          : -((-dy + s_drag_step / 2) / s_drag_step);
    int kind = s_drag_kind;
    int from = s_drag_from;
    lv_obj_t *list = lv_obj_get_parent(card);
    s_drag_kind = 0;
    s_drag_obj = NULL;
    s_drag_end_ms = esp_timer_get_time() / 1000;
    if (list) {
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    }
    int to = from + shift;
    int cnt = (kind == 1) ? s_idx_cnt : s_watch_cnt;
    if (to < 0) to = 0;
    if (to > cnt - 1) to = cnt - 1;
    if (kind == 1) {
        if (to != from) {
            reorder_list(s_idx, s_idx_cnt, from, to);
            idx_save();
            rebuild_idx();
            ui_shell_toast("指数顺序已保存");
        } else {
            rebuild_idx();
        }
    } else if (kind == 2) {
        if (to != from) {
            reorder_list(s_watch, s_watch_cnt, from, to);
            watch_save();
            rebuild_list();
            ui_shell_toast("顺序已保存");
        } else {
            rebuild_list();
        }
    }
}

/* 拖动刚结束的那次 CLICKED 要忽略，否则会顺手打开详情 */
static bool click_suppressed(void)
{
    return (esp_timer_get_time() / 1000 - s_drag_end_ms) < 500;
}

/* ================= 左栏指数 ================= */
static void idx_card_click_cb(lv_event_t *e)
{
    if (click_suppressed()) {
        return;
    }
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) {
        return;                     /* 点的是卡片里的删除按钮 */
    }
    open_idx_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void idx_card_long_cb(lv_event_t *e)
{
    drag_begin(lv_event_get_target(e), 1, (int)(intptr_t)lv_event_get_user_data(e), 84);
}

static void idx_card_pressing_cb(lv_event_t *e)
{
    drag_move(lv_event_get_target(e));
}

static void idx_card_release_cb(lv_event_t *e)
{
    drag_end(lv_event_get_target(e));
}

static void idx_del_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_idx_cnt) {
        return;
    }
    if (s_idx_cnt <= 1) {
        ui_shell_toast("至少留一个指数");
        return;
    }
    for (int i = idx; i < s_idx_cnt - 1; i++) {
        s_idx[i] = s_idx[i + 1];
    }
    s_idx_cnt--;
    memset(&s_idx[s_idx_cnt], 0, sizeof(quote_t));
    idx_save();
    rebuild_idx();
    s_dirty = true;
    ui_shell_toast("已删除");
}

static void rebuild_idx(void)
{
    if (!s_idx_list) {
        return;
    }
    lv_obj_clean(s_idx_list);
    memset(s_idxui, 0, sizeof(s_idxui));

    for (int i = 0; i < s_idx_cnt; i++) {
        lv_obj_t *card = lv_obj_create(s_idx_list);
        lv_obj_set_size(card, 164, 78);
        lv_obj_set_pos(card, 0, i * 84);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_bg_color(card, col(C_BG_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, col(0x1E3358), 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *nm = ui_label(card, s_idx[i].name[0] ? s_idx[i].name : s_idx[i].code,
                                &ui_cjk_16, C_TXT_DIM);
        lv_obj_set_pos(nm, 0, -2);
        lv_obj_set_width(nm, 104);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);

        /* 删除（右上角小 ×） */
        lv_obj_t *del = lv_button_create(card);
        lv_obj_set_size(del, 20, 20);
        lv_obj_align(del, LV_ALIGN_TOP_RIGHT, 2, -4);
        lv_obj_set_style_bg_color(del, col(0x2A2030), 0);
        lv_obj_set_style_bg_opa(del, LV_OPA_60, 0);
        lv_obj_set_style_radius(del, 10, 0);
        lv_obj_set_style_pad_all(del, 0, 0);
        lv_obj_add_event_cb(del, idx_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *dl = ui_label(del, "×", &lv_font_montserrat_14, 0xFF8A8A);
        lv_obj_center(dl);

        lv_obj_t *val = lv_label_create(card);
        lv_label_set_text(val, s_idx[i].valid ? "--" : "----");
        lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(val, col(C_TXT), 0);
        lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        lv_obj_t *pct = ui_label(card, "--", NULL, C_DOWN);
        lv_obj_align(pct, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

        lv_obj_add_event_cb(card, idx_card_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, idx_card_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, idx_card_pressing_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(card, idx_card_release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(card, idx_card_release_cb, LV_EVENT_PRESS_LOST, NULL);

        s_idxui[i].card = card;
        s_idxui[i].name = nm;
        s_idxui[i].val = val;
        s_idxui[i].pct = pct;
    }
    s_dirty = true;
}

/* ================= 个股列表 ================= */
static void del_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_watch_cnt) {
        return;
    }
    char secid[12];
    strlcpy(secid, s_watch[idx].secid, sizeof(secid));
    for (int i = idx; i < s_watch_cnt - 1; i++) {
        s_watch[i] = s_watch[i + 1];
    }
    s_watch_cnt--;
    memset(&s_watch[s_watch_cnt], 0, sizeof(quote_t));
    watch_save();
    alert_drop_stock(secid);
    rebuild_list();
    s_dirty = true;
    ui_shell_toast("已删除");
}

static void rebuild_list(void)
{
    lv_obj_clean(s_list);
    memset(s_rows, 0, sizeof(s_rows));

    int y = 0;
    for (int i = 0; i < s_watch_cnt; i++) {
        lv_obj_t *card = lv_obj_create(s_list);
        lv_obj_set_size(card, 292, 100);
        lv_obj_set_pos(card, 0, y);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_bg_color(card, col(C_BG_CARD2), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, col(0x24406F), 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        /* 名称（16px，字体已含 GB2312 一级字库，直接显示） */
        lv_obj_t *name = ui_label(card, s_watch[i].name[0] ? s_watch[i].name : s_watch[i].code,
                                  &ui_cjk_16, C_TXT);
        lv_obj_set_pos(name, 0, -2);

        /* 代码（右上，避开删除按钮） */
        lv_obj_t *code = ui_label(card, s_watch[i].code, &lv_font_montserrat_14, C_TXT_DIM);
        lv_obj_align(code, LV_ALIGN_TOP_RIGHT, -32, 2);

        /* 现价大字 */
        lv_obj_t *price = lv_label_create(card);
        lv_label_set_text(price, "----");
        lv_obj_set_style_text_font(price, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(price, col(C_CYAN), 0);
        lv_obj_set_pos(price, -2, 22);

        /* 涨跌额 + 涨跌% */
        lv_obj_t *chg = ui_label(card, "--", NULL, C_TXT_DIM);
        lv_obj_align(chg, LV_ALIGN_TOP_RIGHT, -32, 44);

        /* 最高 / 最低 */
        lv_obj_t *hl = ui_label(card, "高 --  低 --", &ui_cjk_16, C_TXT_DIM);
        lv_obj_align(hl, LV_ALIGN_BOTTOM_LEFT, 0, 0);

        /* 删除按钮 */
        lv_obj_t *del = lv_button_create(card);
        lv_obj_set_size(del, 26, 26);
        lv_obj_align(del, LV_ALIGN_TOP_RIGHT, 0, -2);
        lv_obj_set_style_bg_color(del, col(0x3A2030), 0);
        lv_obj_set_style_radius(del, 13, 0);
        lv_obj_set_style_pad_all(del, 0, 0);
        lv_obj_add_event_cb(del, del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        /* 点卡片本身 → 详情；长按拖动 → 调顺序 */
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(card, row_pressing_cb, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(card, row_release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(card, row_release_cb, LV_EVENT_PRESS_LOST, NULL);
        lv_obj_t *dl = ui_label(del, "×", &lv_font_montserrat_16, 0xFF8A8A);
        lv_obj_center(dl);

        s_rows[i].price = price;
        s_rows[i].chg = chg;
        s_rows[i].hl = hl;
        s_rows[i].name = name;

        y += 106;
    }

    if (s_lbl_count) {
        lv_label_set_text_fmt(s_lbl_count, "%d/%d", s_watch_cnt, MAX_WATCH);
    }
    if (s_watch_cnt == 0) {
        lv_obj_remove_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    }
}

static void apply_data(void)
{
    if (!s_visible) {
        return;             /* 界面已销毁（后台关闭时），别碰 LVGL 对象 */
    }
    /* 指数 */
    for (int i = 0; i < s_idx_cnt; i++) {
        if (!s_idx[i].valid || !s_idxui[i].val) {
            continue;
        }
        lv_label_set_text_fmt(s_idxui[i].val, "%.2f", s_idx[i].price);
        lv_label_set_text_fmt(s_idxui[i].pct, "%+.2f%%", s_idx[i].pct);
        lv_obj_set_style_text_color(s_idxui[i].pct, col(pct_color(s_idx[i].pct)), 0);
        lv_obj_set_style_text_color(s_idxui[i].val, col(pct_color(s_idx[i].pct)), 0);
    }
    /* 自选股 */
    for (int i = 0; i < s_watch_cnt; i++) {
        if (!s_rows[i].price || !s_watch[i].valid) {
            continue;
        }
        int dec = (s_watch[i].dec >= 2 && s_watch[i].dec <= 4) ? s_watch[i].dec : 2;
        char b1[32], b2[48], b3[48];
        fmt_price(b1, sizeof(b1), s_watch[i].price, dec);
        lv_label_set_text(s_rows[i].price, b1);
        lv_obj_set_style_text_color(s_rows[i].price, col(pct_color(s_watch[i].pct)), 0);

        snprintf(b2, sizeof(b2), "%+.*f  %+.2f%%", dec, s_watch[i].chg, s_watch[i].pct);
        lv_label_set_text(s_rows[i].chg, b2);
        lv_obj_set_style_text_color(s_rows[i].chg, col(pct_color(s_watch[i].pct)), 0);

        snprintf(b3, sizeof(b3), "%.*f / %.*f", dec, s_watch[i].high, dec, s_watch[i].low);
        lv_label_set_text(s_rows[i].hl, b3);
        if (s_watch[i].name[0]) {
            lv_label_set_text(s_rows[i].name, s_watch[i].name);
        }
    }
    detail_refresh();
    if (s_alist && !lv_obj_has_flag(s_alist, LV_OBJ_FLAG_HIDDEN)) {
        alist_price_refresh();
    }
}


/* ================= 价格提醒 =================
 * 一只股票可以挂多条提醒（不同价位 / 方向 / 有效期）。
 * 有效期按"交易日"算：自动跳过周末与网络交易日历里的法定节假日。
 * 退出 App 后要继续监控，需在 设置→应用管理 给「交易」打开"后台"。
 */
#define ALERT_MAX   16

typedef struct {
    char    secid[12];
    float   target;
    uint8_t dir;        /* 1 涨到 / 2 跌到 */
    uint8_t days;       /* 有效期（交易日）：1 / 30 */
    uint8_t dec;        /* 价格小数位（ETF 3 位） */
    int32_t created;    /* 建立日期 yyyymmdd */
} alert_t;

static alert_t s_alert[ALERT_MAX];
static int s_alert_cnt;

/* 个股详情页 / 提醒列表 / 提醒编辑页 */
static lv_obj_t *s_detail;
static lv_obj_t *s_aedit;
static int  s_detail_idx = -1;
static int  s_detail_kind;        /* 0=个股 1=指数 */
static lv_obj_t *s_dt_alert_btn;  /* 指数没有价格提醒，按钮隐藏 */
static quote_t *detail_quote(void)
{
    int cnt = (s_detail_kind == 1) ? s_idx_cnt : s_watch_cnt;
    if (s_detail_idx < 0 || s_detail_idx >= cnt) {
        return NULL;
    }
    return (s_detail_kind == 1) ? &s_idx[s_detail_idx] : &s_watch[s_detail_idx];
}
static char s_alist_filter[12];     /* 空串 = 显示全部 */
static char s_ae_secid[12];
static uint8_t s_ae_dec;
static uint8_t s_ae_dir;
static uint8_t s_ae_days;
static char s_ae_buf[16];
static lv_obj_t *s_ae_name;
static lv_obj_t *s_ae_now;
static lv_obj_t *s_ae_lbl;
static lv_obj_t *s_ae_dirl[2];
static lv_obj_t *s_ae_daylbl;
static lv_obj_t *s_ae_slider;
static lv_obj_t *s_alist_box;
static lv_obj_t *s_alist_empty;
static lv_obj_t *s_alist_px[ALERT_MAX];   /* 每行"现价"标签，行情刷新时原地更新 */
static lv_obj_t *s_dt_name;
static lv_obj_t *s_dt_code;
static lv_obj_t *s_dt_price;
static lv_obj_t *s_dt_chg;
static lv_obj_t *s_dt_cell[6];      /* 今开 昨收 / 最高 最低 / 量 额 */
static lv_obj_t *s_dt_key[6];       /* 上面 6 格左边的字段名（看历史时要改成 开/收/…） */
static lv_obj_t *s_dt_alerts;

static void alist_rebuild(void);
static void alist_open(const char *secid);
static void aedit_open(const char *secid);

static void alert_save(void)
{
    char buf[768] = { 0 };
    size_t o = 0;
    for (int i = 0; i < s_alert_cnt && o + 64 < sizeof(buf); i++) {
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s%s,%.4f,%d,%d,%d,%d",
                              o ? ";" : "", s_alert[i].secid, s_alert[i].target,
                              s_alert[i].dir, s_alert[i].days,
                              (int)s_alert[i].created, s_alert[i].dec);
    }
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "stk_alert", buf);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void alert_load(void)
{
    s_alert_cnt = 0;
    nvs_handle_t h;
    char buf[768] = { 0 };
    size_t len = sizeof(buf);
    if (nvs_open("sys", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "stk_alert", buf, &len);
        nvs_close(h);
    }
    int today = trade_day_today();
    char *save = NULL;
    for (char *t = strtok_r(buf, ";", &save); t && s_alert_cnt < ALERT_MAX;
         t = strtok_r(NULL, ";", &save)) {
        char *f[6];
        int nf = 0;
        char *p = t;
        while (nf < 6) {
            f[nf++] = p;
            char *c = strchr(p, ',');
            if (!c) {
                break;
            }
            *c = '\0';
            p = c + 1;
        }
        if (nf < 3) {
            continue;
        }
        alert_t a = { 0 };
        strlcpy(a.secid, f[0], sizeof(a.secid));
        a.target = (float)atof(f[1]);
        a.dir = (uint8_t)atoi(f[2]);
        a.days = (nf >= 4) ? (uint8_t)atoi(f[3]) : 30;
        a.created = (nf >= 5) ? (int32_t)atoi(f[4]) : today;
        a.dec = (nf >= 6) ? (uint8_t)atoi(f[5]) : 2;
        if (a.days < 1) {
            a.days = 1;
        }
        if (a.days > 30) {
            a.days = 30;
        }
        if (a.dec < 2 || a.dec > 4) {
            a.dec = 2;
        }
        if (a.dir == 0 || a.target <= 0) {
            continue;
        }
        s_alert[s_alert_cnt++] = a;
    }
    ESP_LOGI(TAG, "alerts loaded: %d", s_alert_cnt);
}

/* 剩余交易日：到期返回 0 */
static int alert_left(const alert_t *a)
{
    if (a->created <= 0) {
        return a->days;
    }
    int used = trade_day_count(a->created, trade_day_today());
    int left = (int)a->days - used;
    return left > 0 ? left : 0;
}

static void alert_remove_at(int i)
{
    if (i < 0 || i >= s_alert_cnt) {
        return;
    }
    for (int k = i; k < s_alert_cnt - 1; k++) {
        s_alert[k] = s_alert[k + 1];
    }
    s_alert_cnt--;
    memset(&s_alert[s_alert_cnt], 0, sizeof(alert_t));
}

static void alert_drop_stock(const char *secid)
{
    bool ch = false;
    for (int i = 0; i < s_alert_cnt;) {
        if (strcmp(s_alert[i].secid, secid) == 0) {
            alert_remove_at(i);
            ch = true;
        } else {
            i++;
        }
    }
    if (ch) {
        alert_save();
    }
}

/* 行情刷新后调用：过期清掉、到价就发提醒 */
static void alert_check(void)
{
    /* App 正在用就照常提醒；退出后要继续监控才需要"后台运行" */
    if (!s_visible && !ui_shell_app_bg_by_name("交易")) {
        return;
    }
    bool changed = false;
    for (int i = 0; i < s_alert_cnt;) {
        alert_t *a = &s_alert[i];
        if (alert_left(a) <= 0) {
            ESP_LOGI(TAG, "alert expired: %s %.3f", a->secid, (double)a->target);
            alert_remove_at(i);
            changed = true;
            continue;
        }
        int wi = find_index_by_secid(a->secid);
        if (wi < 0 || !s_watch[wi].valid) {
            i++;
            continue;
        }
        float p = s_watch[wi].price;
        bool hit = (a->dir == 1) ? (p >= a->target) : (p <= a->target);
        if (!hit) {
            i++;
            continue;
        }
        char pb[16], tb[16], msg[96];
        fmt_price(pb, sizeof(pb), p, a->dec);
        fmt_price(tb, sizeof(tb), a->target, a->dec);
        snprintf(msg, sizeof(msg), "%s 现价 %s / 目标 %s", s_watch[wi].name, pb, tb);
        ui_shell_alert_post(a->dir == 1 ? "涨到目标价" : "跌到目标价", msg);
        ESP_LOGI(TAG, "alert hit: %s %s -> %s", a->secid, pb, tb);
        alert_remove_at(i);
        changed = true;
    }
    if (changed) {
        alert_save();
        /* 注意：本函数跑在抓取任务里（不是 LVGL 线程），
         * 直接动控件会破坏 LVGL 的对象树 → 随机重启。
         * 这里只置标志，交给 tick() 在 LVGL 线程里做。 */
        s_alert_ui_dirty = true;
    }
}

/* 行情轮询：1 秒心跳，按"前台/后台刷新率"决定这次要不要真的去拉
 *   前台（正在用 App）→ stock_cfg_front()（默认 1 秒）
 *   后台（退出 App 但开了"后台"且有未触发的提醒）→ stock_cfg_bg()（默认 3 秒）
 */
static int64_t s_last_poll_ms;

static void stock_poll_cb(void *arg)
{
    (void)arg;
    if (wifi_mgr_get_state() != WIFI_MGR_CONNECTED) {
        return;
    }
    int sec = 0;
    if (s_visible) {
        sec = stock_cfg_front();
    } else if (ui_shell_app_bg_by_name("交易") && s_alert_cnt > 0) {
        sec = stock_cfg_bg();
    }
    if (sec <= 0) {
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    if (s_last_poll_ms > 0 && now - s_last_poll_ms < (int64_t)sec * 1000) {
        return;
    }
    s_last_poll_ms = now;
    fetch_now();
}

/* ================= 个股详情 + 日K =================
 *
 * 日K 用一张 RGB565 画布自绘：
 *   蜡烛（涨=红色空心 / 跌=绿色实心）+ MA5/10/20/60 + 网格 + 十字光标。
 * 价格刻度是 4 个 lv_label（画布里不画字，省得自己造字体光栅化）。
 *
 * 触摸：
 *   按住         → 十字光标选中那天，上面板换成当天的开/收/高/低
 *   按住后左右拖 → 翻看更早的历史（松手后停在原处）
 *   松手         → 回到实时行情
 */

#define KBAR_MAX     180      /* 最多保留 180 根日K（可往前翻） */
#define KVIEW_DEF     50      /* 一屏显示多少根 */
#define KCH_W        380      /* 画布宽（= 绘图区，右侧留白） */
#define KCH_H        196      /* 画布高 */
#define KAXIS_W       52      /* 卡片内左侧刻度区宽度（画布从这里开始） */

typedef struct {
    float o, c, h, l;
    float v;              /* 成交量（手） */
    char  d[11];          /* yyyy-mm-dd */
} kbar_t;

/* K 线周期：日 / 周 / 月 / 年 */
enum { KP_DAY = 0, KP_WEEK, KP_MONTH, KP_YEAR };
static int  s_kperiod = KP_DAY;
static int  s_kbar_period = -1;
static lv_obj_t *s_kp_btn[4];

static const char *kp_name(void)
{
    static const char *n[4] = { "日K", "周K", "月K", "年K" };
    return n[(s_kperiod >= 0 && s_kperiod < 4) ? s_kperiod : 0];
}

static void kline_request_ex(const char *secid, bool force);
static void kline_redraw(void);

static kbar_t s_kbar[KBAR_MAX];
static int    s_kbar_n;
static char   s_kbar_secid[12];
static int64_t s_kbar_ms;          /* 上次抓取时间 */
static char   s_kline_req[12];
static char   s_kline_sym[16];     /* 真正拿去做 K 线的代码（美股带 .OQ） */
static int    s_kline_period = KP_DAY;
static char   s_kline_pending[12];   /* 正在拉 A 时又点了 B：先记下，拉完接着来 */
static volatile bool s_kline_busy;
static int64_t s_kline_req_ms;
static volatile bool s_kline_dirty;
static volatile bool s_kline_fail;   /* 所有源都没拿到 */
static SemaphoreHandle_t s_kline_sem;
static lv_obj_t *s_kimg;         /* K 线画布（一张图，不再一个柱子一个对象） */
static lv_obj_t *s_kmsg;         /* 加载中/失败提示 */
static lv_obj_t *s_k_hdr;        /* 图表标题 / 当天 OHLC */
static lv_obj_t *s_kaxis[4];     /* 左侧价格刻度 */
static uint16_t *s_kbuf;         /* PSRAM 画布 */
static lv_image_dsc_t s_kdsc;

/* 交互状态 */

static int  s_kview_off;         /* 视窗相对"最新"的偏移根数，0 = 跟最新 */
static int  s_ksel = -1;         /* 十字光标选中的 bar 下标，-1 = 实时 */
static int  s_kpress_x;
static int  s_kpress_off;
static bool s_kpan;              /* 本次触摸已转为翻页 */

static void detail_close_cb(lv_event_t *e)
{
    (void)e;
    s_ksel = -1;
    if (s_detail) {
        lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    }
    s_detail_idx = -1;
}

static void detail_alert_cb(lv_event_t *e)
{
    (void)e;
    if (s_detail_idx >= 0 && s_detail_idx < s_watch_cnt) {
        alist_open(s_watch[s_detail_idx].secid);
    }
}

/* ---------------- 日K 绘制 ---------------- */

static inline uint16_t kpix(uint32_t c)
{
    lv_color_t lc = lv_color_hex(c);
    return lv_color_to_u16(lc);
}

static void kfill(int x, int y, int w, int h, uint16_t c)
{
    if (!s_kbuf) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > KCH_W) {
        w = KCH_W - x;
    }
    if (y + h > KCH_H) {
        h = KCH_H - y;
    }
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int r = 0; r < h; r++) {
        uint16_t *row = s_kbuf + (size_t)(y + r) * KCH_W + x;
        for (int i = 0; i < w; i++) {
            row[i] = c;
        }
    }
}

static inline void kdot(int x, int y, uint16_t c)
{
    if (!s_kbuf || x < 0 || x >= KCH_W || y < 0 || y >= KCH_H) {
        return;
    }
    s_kbuf[(size_t)y * KCH_W + x] = c;
}

/* Bresenham 直线：均线用 */
static void kseg(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = (y1 > y0) ? (y0 - y1) : (y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        kdot(x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* 空心矩形（1px 边框）—— 涨的红蜡烛 */
static void kbox(int x, int y, int w, int h, uint16_t c)
{
    if (w <= 2 || h <= 2) {
        kfill(x, y, w, h, c);
        return;
    }
    kfill(x, y, w, 1, c);
    kfill(x, y + h - 1, w, 1, c);
    kfill(x, y, 1, h, c);
    kfill(x + w - 1, y, 1, h, c);
}

static void kdash_h(int y, uint16_t c)
{
    for (int x = 0; x < KCH_W; x += 6) {
        kfill(x, y, 3, 1, c);
    }
}

static void kdash_v(int x, uint16_t c)
{
    for (int y = 0; y < KCH_H; y += 6) {
        kfill(x, y, 1, 3, c);
    }
}

/* 第 gi 根的 p 日均价（收盘价） */
static bool kma_at(int gi, int p, float *out)
{
    if (gi < p - 1 || gi >= s_kbar_n) {
        return false;
    }
    float s = 0;
    for (int k = 0; k < p; k++) {
        s += s_kbar[gi - k].c;
    }
    *out = s / (float)p;
    return true;
}

static int kline_dec(void)
{
    if (s_detail_idx >= 0 && s_detail_idx < s_watch_cnt &&
        s_watch[s_detail_idx].dec >= 2 && s_watch[s_detail_idx].dec <= 4) {
        return s_watch[s_detail_idx].dec;
    }
    return 2;
}

static void kline_redraw(void)
{
    if (!s_kimg || !s_kbuf) {
        return;
    }

    if (s_kbar_n < 2) {
        lv_obj_add_flag(s_kimg, LV_OBJ_FLAG_HIDDEN);
        if (s_kmsg) {
            if (s_kline_fail) {
                lv_label_set_text_fmt(s_kmsg, "%s 获取失败\n点此重试", kp_name());
            } else {
                lv_label_set_text_fmt(s_kmsg, "%s 加载中…", kp_name());
            }
            lv_obj_set_style_text_color(s_kmsg, col(s_kline_fail ? 0xFF8A8A : 0x5B6B85), 0);
            lv_obj_remove_flag(s_kmsg, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_k_hdr) {
            lv_label_set_text(s_k_hdr, kp_name());
        }
        for (int i = 0; i < 4; i++) {
            if (s_kaxis[i]) {
                lv_label_set_text(s_kaxis[i], "");
            }
        }
        return;
    }

    int n = s_kbar_n;
    int vis = (n < KVIEW_DEF) ? n : KVIEW_DEF;
    int maxoff = n - vis;
    if (s_kview_off < 0) {
        s_kview_off = 0;
    }
    if (s_kview_off > maxoff) {
        s_kview_off = maxoff;
    }
    int first = n - vis - s_kview_off;
    int last = first + vis - 1;

    float hi = s_kbar[first].h;
    float lo = s_kbar[first].l;
    for (int i = first + 1; i <= last; i++) {
        if (s_kbar[i].h > hi) hi = s_kbar[i].h;
        if (s_kbar[i].l < lo) lo = s_kbar[i].l;
    }
    float pad = (hi - lo) * 0.08f;
    if (pad < 0.002f) {
        pad = hi * 0.01f + 0.01f;
    }
    hi += pad;
    lo -= pad;
    float span = hi - lo;
    const int ytop = 4;
    const int ybot = KCH_H - 4;

    kfill(0, 0, KCH_W, KCH_H, kpix(0x0C1A33));

    int dec = kline_dec();

    /* 网格 + 左侧价格刻度（4 条） */
    for (int g = 1; g <= 4; g++) {
        int y = ybot - (ybot - ytop) * g / 5;
        kdash_h(y, kpix(0x1B3155));
        if (s_kaxis[g - 1]) {
            char tb[16];
            float pv = lo + span * (float)(ybot - y) / (float)(ybot - ytop);
            snprintf(tb, sizeof(tb), "%.*f", dec, pv);
            lv_label_set_text(s_kaxis[g - 1], tb);
            lv_obj_set_y(s_kaxis[g - 1], 26 + y - 8);
        }
    }

    uint16_t cu = kpix(C_UP);
    uint16_t cd = kpix(C_DOWN);

    /* 蜡烛：用 (i*W/vis) 均分，避免 KCH_W 除不尽时右侧留一条空白 */
    for (int i = first; i <= last; i++) {
        int xa = (i - first) * KCH_W / vis;
        int xb = (i - first + 1) * KCH_W / vis;
        int xc = (xa + xb) / 2;
        int bw = xb - xa - 1;
        if (bw < 1) bw = 1;
        if (bw > 9) bw = 9;

        int yh = ybot - (int)((s_kbar[i].h - lo) * (float)(ybot - ytop) / span);
        int yl = ybot - (int)((s_kbar[i].l - lo) * (float)(ybot - ytop) / span);
        int yo = ybot - (int)((s_kbar[i].o - lo) * (float)(ybot - ytop) / span);
        int yc = ybot - (int)((s_kbar[i].c - lo) * (float)(ybot - ytop) / span);
        bool up = (s_kbar[i].c >= s_kbar[i].o);

        if (yl < yh) {
            int t = yh;
            yh = yl;
            yl = t;
        }
        kfill(xc, yh, 1, yl - yh + 1, up ? cu : cd);      /* 影线 */

        int top = (yo < yc) ? yo : yc;
        int hgt = ((yo > yc) ? (yo - yc) : (yc - yo)) + 1;
        int bx = xc - bw / 2;
        if (bx < 0) {
            bx = 0;
        }
        if (bx + bw > KCH_W) {
            bx = KCH_W - bw;
        }
        if (bx < 0) {
            continue;
        }
        if (up) {
            kbox(bx, top, bw, hgt, cu);                   /* 涨：空心 */
        } else {
            kfill(bx, top, bw, hgt, cd);                  /* 跌：实心 */
        }
    }

    /* 均线 MA5 / MA10 / MA20 / MA60 */
    static const int mperiod[4] = { 5, 10, 20, 60 };
    static const uint32_t mcolor[4] = { 0xD8DEE9, 0xE8A33D, 0xE0609E, 0x4FB477 };
    for (int m = 0; m < 4; m++) {
        int px = -1;
        int py = 0;
        for (int i = first; i <= last; i++) {
            float v;
            if (!kma_at(i, mperiod[m], &v)) {
                px = -1;
                continue;
            }
            int x = ((i - first) * KCH_W + KCH_W / 2) / vis;
            int y = ybot - (int)((v - lo) * (float)(ybot - ytop) / span);
            if (y < ytop) y = ytop;
            if (y > ybot) y = ybot;
            if (px >= 0) {
                kseg(px, py, x, y, kpix(mcolor[m]));
            }
            px = x;
            py = y;
        }
    }

    /* 十字光标 */
    if (s_ksel >= first && s_ksel <= last) {
        int x = ((s_ksel - first) * KCH_W + KCH_W / 2) / vis;
        kdash_v(x, kpix(0x9FB8E0));
        int y = ybot - (int)((s_kbar[s_ksel].c - lo) * (float)(ybot - ytop) / span);
        kdash_h(y, kpix(0x4A6A9E));
    }

    lv_obj_remove_flag(s_kimg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_kimg);
    if (s_kmsg) {
        lv_obj_add_flag(s_kmsg, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_k_hdr) {
        char hb[128];
        if (s_ksel >= 0 && s_ksel < n) {
            kbar_t *k = &s_kbar[s_ksel];
            snprintf(hb, sizeof(hb), "%s", k->d);
        } else {
            snprintf(hb, sizeof(hb), "%s · %d根", kp_name(), vis);
        }
        lv_label_set_text(s_k_hdr, hb);
    }
}

/* ---------------- 触摸：按住看历史 / 左右拖翻页 ---------------- */

static void kline_geom(int *first, int *vis)
{
    int n = s_kbar_n;
    int v = (n < KVIEW_DEF) ? n : KVIEW_DEF;
    if (v < 1) v = 1;
    int f = n - v - s_kview_off;
    if (f < 0) f = 0;
    *first = f;
    *vis = v;
}

static int kline_x_to_idx(int x)
{
    int first, vis;
    kline_geom(&first, &vis);
    int idx = first + x * vis / KCH_W;
    if (idx < first) idx = first;
    if (idx > first + vis - 1) idx = first + vis - 1;
    if (idx > s_kbar_n - 1) idx = s_kbar_n - 1;
    return idx;
}

/* 取触点相对画布左上角的 x（画布外返回 -1） */
static int kline_touch_x(lv_event_t *e, int *in)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return -1;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords((lv_obj_t *)lv_event_get_current_target(e), &a);
    int x = p.x - a.x1;
    int y = p.y - a.y1;
    *in = (y >= 0 && y < KCH_H);
    return x;
}

static void kline_pressed_cb(lv_event_t *e)
{
    if (s_kbar_n < 2) {
        /* 没数据：点一下当重试 */
        if (s_kline_fail && s_detail_idx >= 0 && s_detail_idx < s_watch_cnt) {
            kline_request_ex(s_watch[s_detail_idx].secid, true);
            kline_redraw();
        }
        return;
    }
    int iny = 0;
    int x = kline_touch_x(e, &iny);
    if (x < 0) {
        return;
    }
    if (x < 0) x = 0;
    if (x > KCH_W - 1) x = KCH_W - 1;
    s_kpress_x = x;
    s_kpress_off = s_kview_off;
    s_kpan = false;
    s_ksel = kline_x_to_idx(x);
    detail_refresh();
    kline_redraw();
}

static void kline_pressing_cb(lv_event_t *e)
{
    if (s_kbar_n < 2) {
        return;
    }
    int iny = 0;
    int x = kline_touch_x(e, &iny);
    if (x < 0) {
        x = 0;
    }
    if (x > KCH_W - 1) {
        x = KCH_W - 1;
    }
    int first, vis;
    kline_geom(&first, &vis);

    if (!s_kpan && (x - s_kpress_x > 14 || s_kpress_x - x > 14)) {
        s_kpan = true;
        if (s_ksel >= 0) {
            s_ksel = -1;
            detail_refresh();
        }
    }
    if (s_kpan) {
        int off = s_kpress_off + (x - s_kpress_x) * vis / KCH_W;
        int maxoff = s_kbar_n - vis;
        if (maxoff < 0) maxoff = 0;
        if (off < 0) off = 0;
        if (off > maxoff) off = maxoff;
        if (off != s_kview_off) {
            s_kview_off = off;
            kline_redraw();
        }
        return;
    }
    int idx = kline_x_to_idx(x);
    if (idx != s_ksel) {
        s_ksel = idx;
        detail_refresh();
        kline_redraw();
    }
}

static void kline_released_cb(lv_event_t *e)
{
    (void)e;
    s_kpan = false;
    if (s_ksel >= 0) {
        s_ksel = -1;
        detail_refresh();
        if (s_kimg) {
            kline_redraw();
        }
    }
}

/* ---------------- 抓取 ---------------- */

/* 新浪：[{"day":"2026-11-18","open":"..","high":"..","low":"..","close":"..","volume":".."}]
 *
 * 注意：字段名后面可能带空格（"low": "8.930"），而且 "open"/"high" 是 4 个字母、
 * "low" 只有 3 个 —— 老代码对 low 用 +8 会读成 ".930"，low 变成 0.93，
 * 结果每根蜡烛的影线都拉到画布最底，看起来就是"一排长长的竖线"。
 * 这里改成：找到 key 后跳到 ':' 之后的第一个非引号非空格字符再 atof。 */
static float sina_num(const char *key)
{
    const char *p = strchr(key, ':');
    if (!p) {
        return 0.0f;
    }
    p++;
    while (*p == '"' || *p == ' ') {
        p++;
    }
    return (float)atof(p);
}

static int kline_parse_sina(const char *body, kbar_t *out, int max)
{
    int n = 0;
    const char *p = body;
    while (n < max) {
        const char *d = strstr(p, "\"day\"");
        if (!d) {
            break;
        }
        const char *o = strstr(d, "\"open\"");
        const char *h = strstr(d, "\"high\"");
        const char *l = strstr(d, "\"low\"");
        const char *c = strstr(d, "\"close\"");
        if (!o || !h || !l || !c || !(o < h && h < l && l < c)) {
            break;
        }
        kbar_t k = { 0 };
        k.o = sina_num(o);
        k.h = sina_num(h);
        k.l = sina_num(l);
        k.c = sina_num(c);
        const char *v = strstr(c, "\"volume\"");
        /* 新浪 volume 是"股"，换成"手"（100 股） */
        k.v = v ? (float)(sina_num(v) / 100.0) : 0.0f;
        /* 日期：day 冒号后第一个数字开始取 10 个字符 */
        const char *ds = strchr(d, ':');
        if (ds) {
            ds++;
            while (*ds && (*ds < '0' || *ds > '9')) {
                ds++;
            }
            if (ds[0] && ds[4] == '-' && ds[7] == '-') {
                memcpy(k.d, ds, 10);
                k.d[10] = '\0';
            }
        }
        /* 脏数据直接丢掉（宁可少一根，也不能把坐标拉坏） */
        if (k.o <= 0.0f || k.c <= 0.0f || k.h <= 0.0f || k.l <= 0.0f || k.h < k.l) {
            p = c + 7;
            continue;
        }
        out[n++] = k;
        p = c + 7;
    }
    return n;
}

/* 腾讯：取 ["日期",开,收,高,低,量] */
static const char *k_next_qstr(const char *p, char *out, size_t cap)
{
    p = strchr(p, '"');
    if (!p) {
        return NULL;
    }
    p++;
    const char *e = strchr(p, '"');
    if (!e) {
        return NULL;
    }
    size_t L = (size_t)(e - p);
    if (L >= cap) {
        L = cap - 1;
    }
    memcpy(out, p, L);
    out[L] = '\0';
    return e + 1;
}

/* ---- 东方财富 K 线（日/周/月统一，A 股和美股都能取）----
 * URL: push2his.eastmoney.com/api/qt/stock/kline/get?secid=1.000001&klt=101 ...
 * klines 里每项是 "日期,开,收,高,低,量" */

/* 内部代码 → 东财 secid 候选（美股个股不知道市场，按 105/106/107 试） */
static int em_candidates(const char *sym, char out[][16], int max)
{
    int n = 0;
    if (strncmp(sym, "us", 2) == 0) {
        const char *c = sym + 2;
        if (!strcasecmp(c, "DJI") || !strcasecmp(c, ".DJI")) {
            strlcpy(out[n++], "100.DJIA", 16);
            return n;
        }
        if (!strcasecmp(c, "NDX") || !strcasecmp(c, ".NDX") ||
            !strcasecmp(c, "IXIC") || !strcasecmp(c, ".IXIC")) {
            strlcpy(out[n++], "100.NDX", 16);
            return n;
        }
        if (!strcasecmp(c, "INX") || !strcasecmp(c, ".INX") || !strcasecmp(c, "SPX")) {
            strlcpy(out[n++], "100.SPX", 16);
            return n;
        }
        /* 个股：AAPL.OQ / AAPL.N */
        char code[12];
        const char *dot = strchr(c, '.');
        size_t L = dot ? (size_t)(dot - c) : strlen(c);
        if (L >= sizeof(code)) {
            L = sizeof(code) - 1;
        }
        memcpy(code, c, L);
        code[L] = '\0';
        for (char *p = code; *p; p++) {
            *p = (char)toupper((unsigned char)*p);
        }
        int order[3] = { 105, 106, 107 };     /* 105 纳斯达克 / 106 纽交所 / 107 美交所 */
        if (dot && (dot[1] == 'N' || dot[1] == 'n')) {
            order[0] = 106;
            order[1] = 105;
        }
        for (int i = 0; i < 3 && n < max; i++) {
            snprintf(out[n++], 16, "%d.%s", order[i], code);
        }
        return n;
    }
    if (strncmp(sym, "sh", 2) == 0) {
        snprintf(out[n++], 16, "1.%s", sym + 2);
        return n;
    }
    if (strncmp(sym, "sz", 2) == 0) {
        snprintf(out[n++], 16, "0.%s", sym + 2);
        return n;
    }
    return 0;
}

static int kline_parse_em(const char *body, kbar_t *out, int max)
{
    const char *p = strstr(body, "\"klines\":[");
    if (!p) {
        return 0;
    }
    p = strchr(p, '[');
    int n = 0;
    while (p && n < max) {
        const char *q = strchr(p, '"');
        if (!q) {
            break;
        }
        const char *e = strchr(q + 1, '"');
        if (!e) {
            break;
        }
        char buf[96];
        size_t L = (size_t)(e - q - 1);
        if (L >= sizeof(buf)) {
            L = sizeof(buf) - 1;
        }
        memcpy(buf, q + 1, L);
        buf[L] = '\0';
        char *f[6] = { 0 };
        int nf = 0;
        char *sv = NULL;
        for (char *t = strtok_r(buf, ",", &sv); t && nf < 6; t = strtok_r(NULL, ",", &sv)) {
            f[nf++] = t;
        }
        if (nf >= 6 && strlen(f[0]) == 10 && f[0][4] == '-') {
            out[n].o = (float)atof(f[1]);
            out[n].c = (float)atof(f[2]);
            out[n].h = (float)atof(f[3]);
            out[n].l = (float)atof(f[4]);
            out[n].v = (float)(atof(f[5]) / 100.0);
            memcpy(out[n].d, f[0], 10);
            out[n].d[10] = '\0';
            n++;
        }
        p = e + 1;
    }
    return n;
}

/* key = "day" / "week" / "month"；腾讯复权数据会带 qfq 前缀 */
static int kline_parse(const char *body, kbar_t *out, int max, const char *key)
{
    char k1[24], k2[24];
    snprintf(k1, sizeof(k1), "\"qfq%s\":[[", key);
    snprintf(k2, sizeof(k2), "\"%s\":[[", key);
    const char *p = strstr(body, k1);
    if (!p) {
        p = strstr(body, k2);
    }
    if (!p) {
        return 0;
    }
    int n = 0;
    const char *q = strchr(p, '[');
    while (q && n < max) {
        q = strstr(q, "[\"");
        if (!q) {
            break;
        }
        q++;
        char f[6][24];
        const char *r = q;
        bool ok = true;
        for (int k = 0; k < 6; k++) {
            r = k_next_qstr(r, f[k], sizeof(f[k]));
            if (!r) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            break;
        }
        /* 第一列必须是 yyyy-mm-dd，否则说明已经走出 K 线数组 */
        if (strlen(f[0]) == 10 && f[0][4] == '-' && f[0][7] == '-') {
            out[n].o = (float)atof(f[1]);
            out[n].c = (float)atof(f[2]);
            out[n].h = (float)atof(f[3]);
            out[n].l = (float)atof(f[4]);
            out[n].v = (float)atof(f[5]);       /* 腾讯是"手" */
            memcpy(out[n].d, f[0], 10);
            out[n].d[10] = '\0';
            n++;
        }
        q = r;
    }
    return n;
}

/* 月线 → 年线（同一年的 12 根合并成 1 根） */
static int kline_make_years(const kbar_t *in, int n, kbar_t *out, int max)
{
    int o = 0;
    int i = 0;
    while (i < n && o < max) {
        int y = atoi(in[i].d);
        kbar_t b = in[i];
        float vo = in[i].v;
        int j = i + 1;
        while (j < n && atoi(in[j].d) == y) {
            if (in[j].h > b.h) b.h = in[j].h;
            if (in[j].l < b.l) b.l = in[j].l;
            b.c = in[j].c;
            vo += in[j].v;
            if (in[j].d[0] && j == n - 1) {
                memcpy(b.d, in[j].d, sizeof(b.d));
            }
            j++;
        }
        memcpy(b.d, in[j - 1].d, sizeof(b.d));   /* 用当年最后一个交易日 */
        b.v = vo;
        out[o++] = b;
        i = j;
    }
    return o;
}

/* 通用 HTTP 抓一段文本 */
static int kline_http_get(const char *url, char *buf, int cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .buffer_size = 4096,
        .user_agent = "Mozilla/5.0 (ESP32-P4C5)",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return -1;
    }
    int total = -1;
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        int status = esp_http_client_get_status_code(cli);
        total = 0;
        while (total < cap - 1) {
            int r = esp_http_client_read(cli, buf + total, cap - 1 - total);
            if (r <= 0) {
                break;
            }
            total += r;
        }
        buf[total] = '\0';
        ESP_LOGI(TAG, "GET %s -> http=%d bytes=%d", url, status, total);
    } else {
        ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return total;
}

static void kline_fetch_task(void *arg)
{
    (void)arg;
    static kbar_t tmp[KBAR_MAX + 8];
    char *body = heap_caps_malloc(28000, MALLOC_CAP_SPIRAM);
    if (!body) {
        body = heap_caps_malloc(28000, MALLOC_CAP_DEFAULT);
    }
    ESP_LOGI(TAG, "kline task up, body=%p", body);

    while (1) {
        if (xSemaphoreTake(s_kline_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (!body || wifi_mgr_get_state() != WIFI_MGR_CONNECTED) {
            s_kline_fail = true;
            s_kline_dirty = true;
            s_kline_busy = false;
            continue;
        }
        char url[320];
        int got = 0;
        int total = 0;
        int period = s_kline_period;
        int klt = (period == KP_WEEK) ? 102
                : (period == KP_MONTH || period == KP_YEAR) ? 103 : 101;
        const char *pk = (period == KP_WEEK) ? "week"
                       : (period == KP_MONTH || period == KP_YEAR) ? "month" : "day";
        const char *sym = s_kline_sym[0] ? s_kline_sym : s_kline_req;
        bool is_us = (strncmp(sym, "us", 2) == 0);
        if (is_us && !strchr(sym, '.')) {
            /* 美股 K 线要 "usAAPL.OQ" / "us.DJI"，行情还没回来时先等 */
            ESP_LOGW(TAG, "us kline waiting full code: %s", sym);
            s_kline_fail = true;
            s_kline_dirty = true;
            s_kline_busy = false;
            continue;
        }

        /* ---- 主源：东方财富（日/周/月都支持，A 股美股通用）---- */
        char cand[3][16];
        int nc = em_candidates(sym, cand, 3);
        for (int i = 0; i < nc && got < 2; i++) {
            snprintf(url, sizeof(url),
                     "http://push2his.eastmoney.com/api/qt/stock/kline/get?secid=%s&klt=%d"
                     "&fqt=1&lmt=180&end=20500101&fields1=f1,f2,f3,f4,f5,f6"
                     "&fields2=f51,f52,f53,f54,f55,f56", cand[i], klt);
            total = kline_http_get(url, body, 28000);
            if (total > 0) {
                got = kline_parse_em(body, tmp, KBAR_MAX + 8);
            }
            ESP_LOGI(TAG, "kline em %s %s: bytes=%d bars=%d", cand[i], pk, total, got);
            if (got < 2) {
                vTaskDelay(pdMS_TO_TICKS(150));
            }
        }

        /* ---- 兜底：腾讯（A 股）→ 新浪（仅日线） ---- */
        if (got < 2 && !is_us) {
            snprintf(url, sizeof(url),
                     "http://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=%s,%s,,,180,qfq",
                     sym, pk);
            total = kline_http_get(url, body, 28000);
            if (total > 0) {
                got = kline_parse(body, tmp, KBAR_MAX + 8, pk);
            }
            ESP_LOGI(TAG, "kline tx %s %s: bytes=%d bars=%d", sym, pk, total, got);
        }
        if (got < 2 && period == KP_DAY && !is_us) {
            vTaskDelay(pdMS_TO_TICKS(200));
            snprintf(url, sizeof(url),
                     "http://money.finance.sina.com.cn/quotes_service/api/json_v2.php/"
                     "CN_MarketData.getKLineData?symbol=%s&scale=240&ma=no&datalen=180",
                     sym);
            total = kline_http_get(url, body, 28000);
            if (total > 0) {
                got = kline_parse_sina(body, tmp, KBAR_MAX + 8);
            }
            ESP_LOGI(TAG, "kline sina %s: bytes=%d bars=%d", sym, total, got);
        }

        /* 年K：把月线按年合并 */
        if (period == KP_YEAR && got >= 2) {
            got = kline_make_years(tmp, got, tmp, KBAR_MAX + 8);
            ESP_LOGI(TAG, "kline year bars=%d", got);
        }
        /* 兜底：丢掉明显无效的K线（0/负价），免得一根脏数据把整张图拉坏 */
        {
            int w = 0;
            for (int i = 0; i < got; i++) {
                if (tmp[i].o > 0.0f && tmp[i].c > 0.0f && tmp[i].h > 0.0f &&
                    tmp[i].l > 0.0f && tmp[i].h >= tmp[i].l) {
                    tmp[w++] = tmp[i];
                } else {
                    ESP_LOGW(TAG, "kline drop bad bar: O%.3f H%.3f L%.3f C%.3f",
                             (double)tmp[i].o, (double)tmp[i].h,
                             (double)tmp[i].l, (double)tmp[i].c);
                }
            }
            got = w;
        }
        if (got > KBAR_MAX) {
            memmove(tmp, tmp + (got - KBAR_MAX), sizeof(kbar_t) * (size_t)KBAR_MAX);
            got = KBAR_MAX;
        }
        if (got >= 2) {
            memcpy(s_kbar, tmp, sizeof(kbar_t) * (size_t)got);
            s_kbar_n = got;
            strlcpy(s_kbar_secid, s_kline_req, sizeof(s_kbar_secid));
            s_kbar_period = s_kline_period;
            s_kbar_ms = esp_timer_get_time() / 1000;
            s_kline_fail = false;
            ESP_LOGI(TAG, "kline ok: %d bars %s..%s  last O%.3f H%.3f L%.3f C%.3f",
                     got, s_kbar[0].d, s_kbar[got - 1].d,
                     (double)s_kbar[got - 1].o, (double)s_kbar[got - 1].h,
                     (double)s_kbar[got - 1].l, (double)s_kbar[got - 1].c);
        } else {
            s_kline_fail = true;
        }
        s_kline_dirty = true;
        s_kline_busy = false;
        /* 用户在等待期间切了另一只票：接着拉它 */
        if (s_kline_pending[0]) {
            strlcpy(s_kline_req, s_kline_pending, sizeof(s_kline_req));
            s_kline_pending[0] = '\0';
            s_kline_busy = true;
            s_kline_req_ms = esp_timer_get_time() / 1000;
            s_kline_dirty = true;
            xSemaphoreGive(s_kline_sem);
        }
    }
}

static void kline_task_start(void)
{
    if (s_kline_sem) {
        return;
    }
    s_kline_sem = xSemaphoreCreateBinary();
    if (!s_kline_sem) {
        return;
    }
    BaseType_t ok = xTaskCreateWithCaps(kline_fetch_task, "kline", 14336,
                                        NULL, 2, NULL, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "kline task %s", ok == pdPASS ? "started" : "FAILED");
    if (ok != pdPASS) {
        vSemaphoreDelete(s_kline_sem);
        s_kline_sem = NULL;
    }
}

static void kline_request_ex(const char *sym, bool force)
{
    kline_task_start();
    bool same = (strcmp(s_kbar_secid, sym) == 0 && s_kbar_period == s_kperiod);
    bool fresh = (!force && same && s_kbar_n >= 2 &&
                  esp_timer_get_time() / 1000 - s_kbar_ms < 5 * 60 * 1000);
    if (fresh) {
        kline_redraw();
        return;
    }
    if (s_kline_busy) {
        /* 正在拉别的票/别的周期：记下来，等这一轮结束立刻接上 */
        if (strcmp(s_kline_req, sym) != 0) {
            strlcpy(s_kline_pending, sym, sizeof(s_kline_pending));
        }
        if (!same) {
            s_kbar_n = 0;
            s_kbar_secid[0] = '\0';
            s_ksel = -1;
            s_kview_off = 0;
            kline_redraw();
        }
        return;
    }
    s_kbar_n = 0;
    s_ksel = -1;
    s_kview_off = 0;
    s_kbar_secid[0] = '\0';
    s_kline_fail = false;
    strlcpy(s_kline_req, sym, sizeof(s_kline_req));
    strlcpy(s_kline_sym, sym, sizeof(s_kline_sym));
    s_kline_period = s_kperiod;
    s_kline_busy = true;
    s_kline_req_ms = esp_timer_get_time() / 1000;
    s_kline_dirty = true;
    ESP_LOGI(TAG, "kline request %s (%s)", sym, kp_name());
    if (s_kline_sem) {
        xSemaphoreGive(s_kline_sem);
    }
    kline_redraw();
}

static void kline_request(const char *secid)
{
    kline_request_ex(secid, false);
}

/* ---------------- 详情页 ---------------- */

static void detail_refresh(void)
{
    if (!s_visible || !s_detail) {
        return;
    }
    quote_t *q = detail_quote();
    if (!q) {
        return;
    }

    /* 美股：行情到了才有 "AAPL.OQ"，这时补一次 K 线请求 */
    if (q->valid && strchr(q->ksym, '.') && s_kbar_n < 2 &&
        !s_kline_busy && strcmp(s_kline_req, q->ksym) != 0) {
        kline_request(q->ksym);
    }
    int dec = (q->dec >= 2 && q->dec <= 4) ? q->dec : 2;
    char b[48];

    if (s_ksel >= 0 && s_ksel < s_kbar_n) {
        /* ---- 历史某天：面板整体换成当天的数据 ---- */
        kbar_t *k = &s_kbar[s_ksel];
        float prev = (s_ksel > 0) ? s_kbar[s_ksel - 1].c : k->o;
        float chg = k->c - prev;
        float pct = (prev > 0.0001f) ? (chg / prev * 100.0f) : 0.0f;

        fmt_price(b, sizeof(b), k->c, dec);
        lv_label_set_text(s_dt_price, b);
        lv_obj_set_style_text_color(s_dt_price, col(pct_color(pct)), 0);

        snprintf(b, sizeof(b), "%+.*f    %+.2f%%", dec, chg, pct);
        lv_label_set_text(s_dt_chg, b);
        lv_obj_set_style_text_color(s_dt_chg, col(pct_color(pct)), 0);

        static const char *hk[6] = { "开盘", "收盘", "最高", "最低", "成交量", "涨跌幅" };
        for (int i = 0; i < 6; i++) {
            if (s_dt_key[i]) {
                lv_label_set_text(s_dt_key[i], hk[i]);
            }
        }
        fmt_price(b, sizeof(b), k->o, dec);
        lv_label_set_text(s_dt_cell[0], b);
        fmt_price(b, sizeof(b), k->c, dec);
        lv_label_set_text(s_dt_cell[1], b);
        fmt_price(b, sizeof(b), k->h, dec);
        lv_label_set_text(s_dt_cell[2], b);
        fmt_price(b, sizeof(b), k->l, dec);
        lv_label_set_text(s_dt_cell[3], b);
        if (k->v >= 10000.0f) {
            snprintf(b, sizeof(b), "%.2f万手", k->v / 10000.0f);
        } else if (k->v > 0.0f) {
            snprintf(b, sizeof(b), "%.0f手", k->v);
        } else {
            snprintf(b, sizeof(b), "--");
        }
        lv_label_set_text(s_dt_cell[4], b);
        snprintf(b, sizeof(b), "%+.2f%%", pct);
        lv_label_set_text(s_dt_cell[5], b);
        lv_obj_set_style_text_color(s_dt_cell[5], col(pct_color(pct)), 0);
        return;
    }

    if (s_dt_key[0]) {
        static const char *lk[6] = { "今开", "昨收", "最高", "最低", "成交量", "成交额" };
        for (int i = 0; i < 6; i++) {
            lv_label_set_text(s_dt_key[i], lk[i]);
        }
        lv_obj_set_style_text_color(s_dt_cell[5], col(C_TXT), 0);
    }

    if (!q->valid) {
        return;
    }

    fmt_price(b, sizeof(b), q->price, dec);
    lv_label_set_text(s_dt_price, b);
    lv_obj_set_style_text_color(s_dt_price, col(pct_color(q->pct)), 0);

    snprintf(b, sizeof(b), "%+.*f    %+.2f%%", dec, q->chg, q->pct);
    lv_label_set_text(s_dt_chg, b);
    lv_obj_set_style_text_color(s_dt_chg, col(pct_color(q->pct)), 0);

    fmt_price(b, sizeof(b), q->open, dec);
    lv_label_set_text(s_dt_cell[0], b);
    fmt_price(b, sizeof(b), q->prev, dec);
    lv_label_set_text(s_dt_cell[1], b);
    fmt_price(b, sizeof(b), q->high, dec);
    lv_label_set_text(s_dt_cell[2], b);
    fmt_price(b, sizeof(b), q->low, dec);
    lv_label_set_text(s_dt_cell[3], b);
    if (s_mkt == MKT_US) {
        if (q->vol >= 1e8f) {
            snprintf(b, sizeof(b), "%.2f亿股", q->vol / 1e8f);
        } else {
            snprintf(b, sizeof(b), "%.0f万股", q->vol / 1e4f);
        }
    } else if (q->vol >= 10000.0f) {
        snprintf(b, sizeof(b), "%.2f万手", q->vol / 10000.0f);
    } else {
        snprintf(b, sizeof(b), "%.0f手", q->vol);
    }
    lv_label_set_text(s_dt_cell[4], b);
    if (s_mkt == MKT_US) {
        snprintf(b, sizeof(b), "%.2f亿$", q->amt / 1e8f);
    } else if (q->amt >= 10000.0f) {
        snprintf(b, sizeof(b), "%.2f亿", q->amt / 10000.0f);
    } else {
        snprintf(b, sizeof(b), "%.0f万", q->amt);
    }
    lv_label_set_text(s_dt_cell[5], b);

    int n = 0, minleft = 0;
    for (int i = 0; i < s_alert_cnt; i++) {
        if (strcmp(s_alert[i].secid, q->secid) == 0) {
            int lf = alert_left(&s_alert[i]);
            if (n == 0 || lf < minleft) {
                minleft = lf;
            }
            n++;
        }
    }
    if (s_detail_kind == 1) {
        lv_label_set_text(s_dt_alerts, "指数 · 不支持价格提醒");
        lv_obj_set_style_text_color(s_dt_alerts, col(0x5B6B85), 0);
        return;
    }
    if (n == 0) {
        lv_label_set_text(s_dt_alerts, "未设置价格提醒");
        lv_obj_set_style_text_color(s_dt_alerts, col(0x5B6B85), 0);
    } else {
        lv_label_set_text_fmt(s_dt_alerts, "已设 %d 条 · 最近一条剩 %d 交易日", n, minleft);
        lv_obj_set_style_text_color(s_dt_alerts, col(0xFFD166), 0);
    }
}

static void build_detail(lv_obj_t *parent)
{
    s_detail = lv_obj_create(parent);
    lv_obj_set_size(s_detail, 480, 480);
    lv_obj_set_pos(s_detail, 0, 0);
    lv_obj_set_style_bg_color(s_detail, col(0x05101F), 0);
    lv_obj_set_style_bg_opa(s_detail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_detail, 0, 0);
    lv_obj_set_style_radius(s_detail, 0, 0);
    lv_obj_set_style_pad_all(s_detail, 0, 0);
    lv_obj_clear_flag(s_detail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *back = lv_button_create(s_detail);
    lv_obj_set_size(back, 90, 38);
    lv_obj_set_pos(back, 10, 3);
    lv_obj_set_style_radius(back, 19, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_style_bg_color(back, col(0x1E2C4E), 0);
    lv_obj_add_event_cb(back, detail_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(back, "返回", &ui_cjk_16, C_TXT);
    lv_obj_center(bl);

    s_dt_name = ui_label(s_detail, "-", &ui_cjk_24, C_TXT);
    lv_obj_set_pos(s_dt_name, 112, 6);
    s_dt_code = ui_label(s_detail, "", NULL, C_TXT_DIM);
    lv_obj_align(s_dt_code, LV_ALIGN_TOP_RIGHT, -14, 14);

    s_dt_price = lv_label_create(s_detail);
    lv_label_set_text(s_dt_price, "----");
    lv_obj_set_style_text_font(s_dt_price, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_dt_price, col(C_TXT), 0);
    lv_obj_set_pos(s_dt_price, 14, 44);

    s_dt_chg = ui_label(s_detail, "--", &ui_cjk_16, C_TXT_DIM);
    lv_obj_align(s_dt_chg, LV_ALIGN_TOP_RIGHT, -14, 58);

    /* 6 个数据格：3 列 × 2 行 */
    static const char *k1[3] = { "今开", "最高", "成交量" };
    static const char *k2[3] = { "昨收", "最低", "成交额" };
    for (int i = 0; i < 3; i++) {
        int x = 12 + i * 153;
        lv_obj_t *c1 = ui_card(s_detail, 150, 34, C_BG_CARD);
        lv_obj_set_style_pad_all(c1, 2, 0);
        lv_obj_set_pos(c1, x, 88);
        s_dt_key[i * 2] = ui_label(c1, k1[i], NULL, C_TXT_DIM);
        lv_obj_align(s_dt_key[i * 2], LV_ALIGN_LEFT_MID, 6, 0);
        s_dt_cell[i * 2] = ui_label(c1, "--", NULL, C_TXT);
        lv_obj_align(s_dt_cell[i * 2], LV_ALIGN_RIGHT_MID, -6, 0);

        lv_obj_t *c2 = ui_card(s_detail, 150, 34, C_BG_CARD);
        lv_obj_set_style_pad_all(c2, 2, 0);
        lv_obj_set_pos(c2, x, 126);
        s_dt_key[i * 2 + 1] = ui_label(c2, k2[i], NULL, C_TXT_DIM);
        lv_obj_align(s_dt_key[i * 2 + 1], LV_ALIGN_LEFT_MID, 6, 0);
        s_dt_cell[i * 2 + 1] = ui_label(c2, "--", NULL, C_TXT);
        lv_obj_align(s_dt_cell[i * 2 + 1], LV_ALIGN_RIGHT_MID, -6, 0);
    }

    /* ---- 日K ---- */
    lv_obj_t *kc = ui_card(s_detail, 456, 230, C_BG_CARD);
    lv_obj_set_pos(kc, 12, 164);
    lv_obj_set_style_pad_all(kc, 0, 0);

    s_k_hdr = ui_label(kc, "日K", NULL, C_TXT_DIM);
    lv_obj_set_pos(s_k_hdr, 10, 4);
    lv_obj_set_width(s_k_hdr, 246);
    lv_label_set_long_mode(s_k_hdr, LV_LABEL_LONG_DOT);

    /* 右上角：日 / 周 / 月 / 年 */
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_button_create(kc);
        lv_obj_set_size(b, 44, 22);
        lv_obj_set_pos(b, 262 + i * 48, 3);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, col(i == s_kperiod ? 0x8A3AB2 : 0x16203A), 0);
        lv_obj_add_event_cb(b, kperiod_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        static const char *tn[4] = { "日", "周", "月", "年" };
        lv_obj_t *l = ui_label(b, tn[i], &ui_cjk_16, i == s_kperiod ? C_TXT : 0x6B7F9E);
        lv_obj_center(l);
        s_kp_btn[i] = b;
    }

    /* 左侧价格刻度（4 个，画布里不画字） */
    for (int i = 0; i < 4; i++) {
        s_kaxis[i] = lv_label_create(kc);
        lv_label_set_text(s_kaxis[i], "");
        lv_obj_set_style_text_font(s_kaxis[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_kaxis[i], col(0x6B7F9E), 0);
        lv_obj_set_style_text_align(s_kaxis[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(s_kaxis[i], 46, 16);
        lv_obj_set_pos(s_kaxis[i], 0, 60);
    }

    if (!s_kbuf) {
        s_kbuf = heap_caps_malloc(KCH_W * KCH_H * 2, MALLOC_CAP_SPIRAM);
        if (!s_kbuf) {
            s_kbuf = heap_caps_malloc(KCH_W * KCH_H * 2, MALLOC_CAP_DEFAULT);
        }
        ESP_LOGI(TAG, "kline canvas buf=%p (%d KB)", s_kbuf, KCH_W * KCH_H * 2 / 1024);
    }
    if (s_kbuf) {
        memset(s_kbuf, 0, KCH_W * KCH_H * 2);
        memset(&s_kdsc, 0, sizeof(s_kdsc));
        /* LVGL 9 要求 magic，否则 lv_image_src_get_type() 会判成 UNKNOWN 直接不画 */
        s_kdsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_kdsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_kdsc.header.w = KCH_W;
        s_kdsc.header.h = KCH_H;
        s_kdsc.header.stride = KCH_W * 2;
        s_kdsc.data = (const uint8_t *)s_kbuf;
        s_kdsc.data_size = KCH_W * KCH_H * 2;
    }
    s_kimg = lv_image_create(kc);
    lv_obj_set_pos(s_kimg, KAXIS_W, 26);
    if (s_kbuf) {
        lv_image_set_src(s_kimg, &s_kdsc);
    }
    lv_obj_add_flag(s_kimg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_kimg, kline_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_kimg, kline_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_kimg, kline_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_kimg, kline_released_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_flag(s_kimg, LV_OBJ_FLAG_HIDDEN);

    s_kmsg = ui_label(kc, "日K 加载中…", NULL, 0x5B6B85);
    lv_obj_set_style_text_align(s_kmsg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_kmsg, 130, 110);
    lv_obj_add_flag(s_kmsg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_kmsg, kline_pressed_cb, LV_EVENT_PRESSED, NULL);

    s_dt_alerts = ui_label(s_detail, "", NULL, 0x5B6B85);
    lv_obj_set_pos(s_dt_alerts, 14, 396);

    lv_obj_t *ab = lv_button_create(s_detail);
    s_dt_alert_btn = ab;
    lv_obj_set_size(ab, 456, 40);
    lv_obj_set_pos(ab, 12, 416);
    lv_obj_set_style_radius(ab, 12, 0);
    lv_obj_set_style_bg_color(ab, col(0x8A3AB2), 0);
    lv_obj_add_event_cb(ab, detail_alert_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *abl = ui_label(ab, "价格提醒（可设多条）", &ui_cjk_16, C_TXT);
    lv_obj_center(abl);
}

static void kperiod_cb(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    if (p < 0 || p > 3 || p == s_kperiod) {
        return;
    }
    s_kperiod = p;
    for (int i = 0; i < 4; i++) {
        if (!s_kp_btn[i]) {
            continue;
        }
        lv_obj_set_style_bg_color(s_kp_btn[i], col(i == p ? 0x8A3AB2 : 0x16203A), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_kp_btn[i], 0),
                                    col(i == p ? C_TXT : 0x6B7F9E), 0);
    }
    s_kview_off = 0;
    s_ksel = -1;
    quote_t *q = detail_quote();
    if (q) {
        kline_request_ex(q->ksym[0] ? q->ksym : q->secid, true);
    }
}

static void detail_open(int kind, int idx)
{
    int cnt = (kind == 1) ? s_idx_cnt : s_watch_cnt;
    if (!s_detail || idx < 0 || idx >= cnt) {
        ESP_LOGW(TAG, "detail rejected kind=%d idx=%d cnt=%d", kind, idx, cnt);
        return;
    }
    s_detail_kind = kind;
    s_detail_idx = idx;
    s_ksel = -1;
    s_kview_off = 0;
    quote_t *q = (kind == 1) ? &s_idx[idx] : &s_watch[idx];
    lv_label_set_text(s_dt_name, q->name[0] ? q->name : q->code);
    lv_label_set_text(s_dt_code, q->code);
    if (s_dt_alert_btn) {
        if (kind == 1) {
            lv_obj_add_flag(s_dt_alert_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_dt_alert_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    detail_refresh();
    kline_request(q->ksym[0] ? q->ksym : q->secid);
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_detail);
}

static void open_detail(int idx)
{
    detail_open(0, idx);
}

static void open_idx_detail(int idx)
{
    detail_open(1, idx);
}

static void row_click_cb(lv_event_t *e)
{
    if (click_suppressed()) {
        return;
    }
    open_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void row_long_cb(lv_event_t *e)
{
    drag_begin(lv_event_get_target(e), 2, (int)(intptr_t)lv_event_get_user_data(e), 106);
}

static void row_pressing_cb(lv_event_t *e)
{
    drag_move(lv_event_get_target(e));
}

static void row_release_cb(lv_event_t *e)
{
    drag_end(lv_event_get_target(e));
}

/* ================= 提醒列表 ================= */

/* 行情刷新时只改"现价"文字，不重建列表（否则会把滚动位置弄丢） */
static void alist_price_refresh(void)
{
    for (int i = 0; i < s_alert_cnt && i < ALERT_MAX; i++) {
        if (!s_alist_px[i]) {
            continue;
        }
        int wi = find_index_by_secid(s_alert[i].secid);
        char b[32] = "现价 --";
        if (wi >= 0 && s_watch[wi].valid) {
            char p[16];
            fmt_price(p, sizeof(p), s_watch[wi].price, s_alert[i].dec);
            snprintf(b, sizeof(b), "现价 %s", p);
        }
        lv_label_set_text(s_alist_px[i], b);
    }
}

static void alist_del_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_alert_cnt) {
        return;
    }
    alert_remove_at(idx);
    alert_save();
    alist_rebuild();
    if (s_detail) {
        detail_refresh();
    }
    ui_shell_toast("已删除提醒");
}

static void alist_rebuild(void)
{
    if (!s_alist_box) {
        return;
    }
    lv_obj_clean(s_alist_box);
    memset(s_alist_px, 0, sizeof(s_alist_px));
    int y = 0;
    int shown = 0;

    for (int i = 0; i < s_alert_cnt; i++) {
        alert_t *a = &s_alert[i];
        if (s_alist_filter[0] && strcmp(a->secid, s_alist_filter) != 0) {
            continue;
        }
        const char *nm = a->secid + 2;
        int wi = find_index_by_secid(a->secid);
        if (wi >= 0 && s_watch[wi].name[0]) {
            nm = s_watch[wi].name;
        }

        lv_obj_t *card = lv_obj_create(s_alist_box);
        lv_obj_set_size(card, 456, 64);
        lv_obj_set_pos(card, 0, y);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_bg_color(card, col(C_BG_CARD2), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, col(0x24406F), 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nl = ui_label(card, nm, &ui_cjk_16, C_TXT);
        lv_obj_set_pos(nl, 0, 0);

        char tb[16], ln[64];
        fmt_price(tb, sizeof(tb), a->target, a->dec);
        snprintf(ln, sizeof(ln), "%s %s", a->dir == 1 ? "涨到" : "跌到", tb);
        lv_obj_t *tl = ui_label(card, ln, &ui_cjk_16, a->dir == 1 ? C_UP : C_DOWN);
        lv_obj_set_pos(tl, 0, 28);

        int lf = alert_left(a);
        char rb[32];
        if (lf > 0) {
            snprintf(rb, sizeof(rb), "剩 %d 交易日", lf);
        } else {
            snprintf(rb, sizeof(rb), "已到期");
        }
        lv_obj_t *rl = ui_label(card, rb, NULL, lf > 0 ? 0xFFD166 : 0x8FA8CC);
        lv_obj_align(rl, LV_ALIGN_BOTTOM_RIGHT, -36, 0);

        /* 现价：跟着行情实时刷新（原地改文字，不重建列表） */
        int wi2 = find_index_by_secid(a->secid);
        char pxb[32] = "现价 --";
        if (wi2 >= 0 && s_watch[wi2].valid) {
            char pxs[16];
            fmt_price(pxs, sizeof(pxs), s_watch[wi2].price, a->dec);
            snprintf(pxb, sizeof(pxb), "现价 %s", pxs);
        }
        lv_obj_t *pxl = ui_label(card, pxb, NULL, 0x8FA8CC);
        lv_obj_align(pxl, LV_ALIGN_TOP_RIGHT, -36, 2);
        s_alist_px[i] = pxl;

        lv_obj_t *del = lv_button_create(card);
        lv_obj_set_size(del, 26, 26);
        lv_obj_align(del, LV_ALIGN_TOP_RIGHT, 0, -2);
        lv_obj_set_style_bg_color(del, col(0x3A2030), 0);
        lv_obj_set_style_radius(del, 13, 0);
        lv_obj_set_style_pad_all(del, 0, 0);
        lv_obj_add_event_cb(del, alist_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *dl = ui_label(del, "×", &lv_font_montserrat_16, 0xFF8A8A);
        lv_obj_center(dl);

        y += 70;
        shown++;
    }

    if (s_alist_empty) {
        if (shown == 0) {
            lv_obj_remove_flag(s_alist_empty, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_alist_empty, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void alist_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_alist) {
        lv_obj_add_flag(s_alist, LV_OBJ_FLAG_HIDDEN);
    }
}

static void alist_add_cb(lv_event_t *e)
{
    (void)e;
    if (!s_alist_filter[0]) {
        ui_shell_toast("请从个股页面添加提醒");
        return;
    }
    aedit_open(s_alist_filter);
}

static void alist_open(const char *secid)
{
    if (!s_alist) {
        return;
    }
    strlcpy(s_alist_filter, secid ? secid : "", sizeof(s_alist_filter));
    alist_rebuild();
    lv_obj_remove_flag(s_alist, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_alist);
}

/* 主界面右上角铃铛：查看全部提醒 */
static void alist_open_all_cb(lv_event_t *e)
{
    (void)e;
    alist_open("");
}

static void build_alert_list(lv_obj_t *parent)
{
    s_alist = lv_obj_create(parent);
    lv_obj_set_size(s_alist, 480, 480);
    lv_obj_set_pos(s_alist, 0, 0);
    lv_obj_set_style_bg_color(s_alist, col(0x05101F), 0);
    lv_obj_set_style_bg_opa(s_alist, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_alist, 0, 0);
    lv_obj_set_style_radius(s_alist, 0, 0);
    lv_obj_set_style_pad_all(s_alist, 0, 0);
    lv_obj_clear_flag(s_alist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_alist, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *t = ui_label(s_alist, "提醒列表", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(t, 16, 12);

    lv_obj_t *close = lv_button_create(s_alist);
    lv_obj_set_size(close, 76, 32);
    lv_obj_set_pos(close, 388, 6);
    lv_obj_set_style_radius(close, 16, 0);
    lv_obj_set_style_pad_all(close, 0, 0);
    lv_obj_set_style_bg_color(close, col(0x2E4C7A), 0);
    lv_obj_add_event_cb(close, alist_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = ui_label(close, "关闭", &ui_cjk_16, C_TXT);
    lv_obj_center(cl);

    s_alist_box = lv_obj_create(s_alist);
    lv_obj_set_size(s_alist_box, 464, 372);
    lv_obj_set_pos(s_alist_box, 8, 46);
    lv_obj_set_style_bg_opa(s_alist_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_alist_box, 0, 0);
    lv_obj_set_style_pad_all(s_alist_box, 0, 0);
    lv_obj_set_style_pad_bottom(s_alist_box, 10, 0);
    lv_obj_set_scroll_dir(s_alist_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_alist_box, LV_SCROLLBAR_MODE_AUTO);

    s_alist_empty = ui_label(s_alist, "还没有设置提醒\n在个股页面点「价格提醒」添加", NULL, 0x5B6B85);
    lv_obj_set_style_text_align(s_alist_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_alist_empty, 440);
    lv_obj_align(s_alist_empty, LV_ALIGN_TOP_MID, 0, 190);

    lv_obj_t *add = lv_button_create(s_alist);
    lv_obj_set_size(add, 456, 46);
    lv_obj_set_pos(add, 12, 428);
    lv_obj_set_style_radius(add, 12, 0);
    lv_obj_set_style_bg_color(add, col(0x8A3AB2), 0);
    lv_obj_add_event_cb(add, alist_add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = ui_label(add, "添加提醒", &ui_cjk_16, C_TXT);
    lv_obj_center(al);
}

/* ================= 提醒编辑 ================= */

static void aedit_dir_refresh(void)
{
    for (int k = 0; k < 2; k++) {
        if (!s_ae_dirl[k]) {
            continue;
        }
        bool on = (s_ae_dir == (uint8_t)(k + 1));
        lv_obj_set_style_text_color(s_ae_dirl[k], col(on ? 0xFFD166 : 0x8FA8CC), 0);
        lv_obj_t *b = lv_obj_get_parent(s_ae_dirl[k]);
        if (b) {
            lv_obj_set_style_bg_color(b, col(on ? 0x2E4C7A : 0x16203A), 0);
        }
    }
}

static void aedit_day_slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    s_ae_days = (uint8_t)lv_slider_get_value(sl);
    if (s_ae_daylbl) {
        lv_label_set_text_fmt(s_ae_daylbl, "%d 交易日", s_ae_days);
    }
}

static void aedit_dir_cb(lv_event_t *e)
{
    s_ae_dir = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    aedit_dir_refresh();
}

static void aedit_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_aedit) {
        lv_obj_add_flag(s_aedit, LV_OBJ_FLAG_HIDDEN);
    }
}

static void aedit_save(void)
{
    float v = (float)atof(s_ae_buf);
    if (v <= 0) {
        ui_shell_toast("请输入目标价");
        return;
    }
    if (s_alert_cnt >= ALERT_MAX) {
        ui_shell_toast("提醒数量已满");
        return;
    }
    /* 同股票 + 同方向 + 同价：覆盖有效期，不重复添加 */
    for (int i = 0; i < s_alert_cnt; i++) {
        if (strcmp(s_alert[i].secid, s_ae_secid) == 0 &&
            s_alert[i].dir == s_ae_dir && fabsf(s_alert[i].target - v) < 0.0005f) {
            s_alert[i].days = s_ae_days;
            s_alert[i].created = trade_day_today();
            s_alert[i].dec = s_ae_dec;
            alert_save();
            aedit_close_cb(NULL);
            alist_rebuild();
            if (s_detail) {
                detail_refresh();
            }
            ui_shell_toast("已更新提醒");
            return;
        }
    }

    alert_t *a = &s_alert[s_alert_cnt++];
    memset(a, 0, sizeof(*a));
    strlcpy(a->secid, s_ae_secid, sizeof(a->secid));
    a->target = v;
    a->dir = s_ae_dir;
    a->days = s_ae_days;
    a->dec = s_ae_dec;
    a->created = trade_day_today();
    alert_save();

    aedit_close_cb(NULL);
    alist_rebuild();
    if (s_detail) {
        detail_refresh();
    }
    if (ui_shell_app_bg_by_name("交易")) {
        ui_shell_toast("已设置，到价会弹窗");
    } else {
        ui_shell_toast("已设置（需开后台才会在退出后提醒）");
    }
}

static void aedit_save_cb(lv_event_t *e)
{
    (void)e;
    aedit_save();
}

static void aedit_open(const char *secid)
{
    if (!s_aedit || !secid || !secid[0]) {
        return;
    }
    int wi = find_index_by_secid(secid);
    if (wi < 0) {
        ui_shell_toast("先在自选里添加该股票");
        return;
    }
    strlcpy(s_ae_secid, secid, sizeof(s_ae_secid));
    s_ae_dec = (uint8_t)((s_watch[wi].dec >= 2 && s_watch[wi].dec <= 4) ? s_watch[wi].dec : 2);
    s_ae_dir = 1;
    s_ae_days = 30;
    snprintf(s_ae_buf, sizeof(s_ae_buf), "%.*f", s_ae_dec, (double)s_watch[wi].price);

    lv_label_set_text(s_ae_name, s_watch[wi].name);
    char nb[48];
    snprintf(nb, sizeof(nb), "现价 %.*f", s_ae_dec, (double)s_watch[wi].price);
    lv_label_set_text(s_ae_now, nb);
    lv_label_set_text(s_ae_lbl, s_ae_buf);
    ui_keypad_bind(s_ae_buf, sizeof(s_ae_buf), s_ae_lbl, NULL, aedit_save);

    aedit_dir_refresh();
    if (s_ae_slider) {
        lv_slider_set_value(s_ae_slider, s_ae_days, LV_ANIM_OFF);
    }
    if (s_ae_daylbl) {
        lv_label_set_text_fmt(s_ae_daylbl, "%d 交易日", s_ae_days);
    }
    lv_obj_remove_flag(s_aedit, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_aedit);
}

static void build_alert_edit(lv_obj_t *parent)
{
    s_aedit = lv_obj_create(parent);
    lv_obj_set_size(s_aedit, 480, 480);
    lv_obj_set_pos(s_aedit, 0, 0);
    lv_obj_set_style_bg_color(s_aedit, col(0x0B1220), 0);
    lv_obj_set_style_bg_opa(s_aedit, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_aedit, 0, 0);
    lv_obj_set_style_radius(s_aedit, 0, 0);
    lv_obj_set_style_pad_all(s_aedit, 0, 0);
    lv_obj_clear_flag(s_aedit, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_aedit, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *t = ui_label(s_aedit, "添加价格提醒", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(t, 16, 12);

    lv_obj_t *close = lv_button_create(s_aedit);
    lv_obj_set_size(close, 76, 32);
    lv_obj_set_pos(close, 388, 6);
    lv_obj_set_style_radius(close, 16, 0);
    lv_obj_set_style_pad_all(close, 0, 0);
    lv_obj_set_style_bg_color(close, col(0x2E4C7A), 0);
    lv_obj_add_event_cb(close, aedit_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = ui_label(close, "关闭", &ui_cjk_16, C_TXT);
    lv_obj_center(cl);

    s_ae_name = ui_label(s_aedit, "-", &ui_cjk_16, 0xFFD166);
    lv_obj_set_pos(s_ae_name, 16, 48);

    s_ae_now = ui_label(s_aedit, "现价 --", NULL, C_TXT_DIM);
    lv_obj_set_pos(s_ae_now, 16, 76);

    lv_obj_t *tl = ui_label(s_aedit, "目标价", &ui_cjk_16, C_TXT_DIM);
    lv_obj_set_pos(tl, 16, 112);

    lv_obj_t *box = lv_obj_create(s_aedit);
    lv_obj_set_size(box, 130, 40);
    lv_obj_set_pos(box, 84, 104);
    lv_obj_set_style_bg_color(box, col(0x16203A), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, col(0x2E4C7A), 0);
    lv_obj_set_style_radius(box, 8, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    s_ae_lbl = ui_label(box, "0.00", &lv_font_montserrat_20, 0xFFD166);
    lv_obj_center(s_ae_lbl);

    static const char *dtxt[2] = { "涨到", "跌到" };
    for (int k = 0; k < 2; k++) {
        lv_obj_t *b = lv_button_create(s_aedit);
        lv_obj_set_size(b, 80, 40);
        lv_obj_set_pos(b, 226 + k * 88, 104);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, col(0x16203A), 0);
        lv_obj_add_event_cb(b, aedit_dir_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(k + 1));
        s_ae_dirl[k] = ui_label(b, dtxt[k], &ui_cjk_16, 0x8FA8CC);
        lv_obj_center(s_ae_dirl[k]);
    }

    lv_obj_t *dl = ui_label(s_aedit, "有效期", &ui_cjk_16, C_TXT_DIM);
    lv_obj_set_pos(dl, 16, 164);

    /* 1~30 个交易日，直接拖 */
    s_ae_slider = lv_slider_create(s_aedit);
    lv_obj_set_size(s_ae_slider, 236, 16);
    lv_obj_set_pos(s_ae_slider, 84, 168);
    lv_slider_set_range(s_ae_slider, 1, 30);
    lv_slider_set_value(s_ae_slider, 30, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_ae_slider, aedit_day_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_ae_daylbl = ui_label(s_aedit, "30 交易日", &ui_cjk_16, 0xFFD166);
    lv_obj_set_pos(s_ae_daylbl, 332, 162);

    lv_obj_t *hint = ui_label(s_aedit, "按交易日算", NULL, 0x5B6B85);
    lv_obj_set_pos(hint, 16, 194);

    static const char *rows[4] = { "123", "456", "789", ".0-" };
    ui_keypad_build(s_aedit, 216, rows, 4);

    lv_obj_t *save = lv_button_create(s_aedit);
    lv_obj_set_size(save, 160, 40);
    lv_obj_set_pos(save, 16, 434);
    lv_obj_set_style_radius(save, 20, 0);
    lv_obj_set_style_bg_color(save, col(0x2E7D5B), 0);
    lv_obj_add_event_cb(save, aedit_save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = ui_label(save, "保存提醒", &ui_cjk_16, C_TXT);
    lv_obj_center(sl);

    lv_obj_t *cancel = lv_button_create(s_aedit);
    lv_obj_set_size(cancel, 120, 40);
    lv_obj_set_pos(cancel, 190, 434);
    lv_obj_set_style_radius(cancel, 20, 0);
    lv_obj_set_style_bg_color(cancel, col(0x3A2030), 0);
    lv_obj_add_event_cb(cancel, aedit_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl2 = ui_label(cancel, "取消", &ui_cjk_16, 0xFF8A8A);
    lv_obj_center(cl2);
}

/* ---- 添加弹窗 ---- */

/* ================= 添加：先选「个股 / 指数」，再输代码 ================= */
static int s_add_mode;          /* 0=个股 1=指数 */
static lv_obj_t *s_add_title;
static lv_obj_t *s_add_hint;
static lv_obj_t *s_add_mode_btn[2];

static void add_close_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_add_modal, LV_OBJ_FLAG_HIDDEN);
}

static void add_mode_refresh(void)
{
    for (int i = 0; i < 2; i++) {
        if (!s_add_mode_btn[i]) {
            continue;
        }
        bool on = (i == s_add_mode);
        lv_obj_set_style_bg_color(s_add_mode_btn[i], col(on ? 0x8A3AB2 : 0x16203A), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_add_mode_btn[i], 0),
                                    col(on ? C_TXT : 0x6B7F9E), 0);
    }
    if (s_add_title) {
        lv_label_set_text(s_add_title, s_add_mode ? "添加指数（左栏）" : "添加个股（右栏）");
    }
    if (s_add_hint) {
        if (s_mkt == MKT_US) {
            lv_label_set_text(s_add_hint, s_add_mode ? "美股指数代码，如 DJI / IXIC"
                                                    : "美股代码，如 AAPL / NVDA");
        } else {
            lv_label_set_text(s_add_hint, s_add_mode ? "指数代码，如 000001 / 399001"
                                                    : "6 位股票代码，如 600519");
        }
    }
}

static void add_mode_cb(lv_event_t *e)
{
    s_add_mode = (int)(intptr_t)lv_event_get_user_data(e);
    lv_textarea_set_text(s_add_ta, "");
    add_mode_refresh();
}

static void add_ok_cb(lv_event_t *e)
{
    (void)e;
    const char *code = lv_textarea_get_text(s_add_ta);
    if (!code[0]) {
        ui_shell_toast("请输入代码");
        return;
    }
    if (s_mkt == MKT_A && strlen(code) != 6) {
        ui_shell_toast("A 股请输入 6 位代码");
        return;
    }
    char secid[16];
    code_to_secid(code, secid, sizeof(secid));

    if (s_add_mode == 1) {
        /* ---- 加指数：左栏 ---- */
        for (int i = 0; i < s_idx_cnt; i++) {
            if (strcmp(s_idx[i].secid, secid) == 0) {
                ui_shell_toast("该指数已在左栏");
                return;
            }
        }
        if (s_idx_cnt >= MAX_IDX) {
            ui_shell_toast("指数最多 6 个，请先删除");
            return;
        }
        quote_t *q = &s_idx[s_idx_cnt];
        memset(q, 0, sizeof(*q));
        strlcpy(q->secid, secid, sizeof(q->secid));
        strlcpy(q->code, code, sizeof(q->code));
        strlcpy(q->ksym, secid, sizeof(q->ksym));
        q->dec = 2;
        s_idx_cnt++;
        idx_save();
        rebuild_idx();
        lv_obj_add_flag(s_add_modal, LV_OBJ_FLAG_HIDDEN);
        ui_shell_toast("已添加指数，正在获取行情…");
        fetch_now();
        return;
    }

    /* ---- 加个股：右栏 ---- */
    if (find_index_by_secid(secid) >= 0) {
        ui_shell_toast("该股票已在自选里");
        return;
    }
    if (s_watch_cnt >= MAX_WATCH) {
        ui_shell_toast("最多 8 只，请先删除");
        return;
    }
    quote_t *w = &s_watch[s_watch_cnt];
    memset(w, 0, sizeof(*w));
    strlcpy(w->secid, secid, sizeof(w->secid));
    strlcpy(w->code, code, sizeof(w->code));
    strlcpy(w->ksym, secid, sizeof(w->ksym));
    s_watch_cnt++;
    watch_save();
    rebuild_list();
    lv_obj_add_flag(s_add_modal, LV_OBJ_FLAG_HIDDEN);
    ui_shell_toast("已添加，正在获取行情…");
    fetch_now();
}

static void add_btn_cb(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_add_ta, "");
    add_mode_refresh();
    lv_obj_remove_flag(s_add_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_add_modal);
}

static void build_add_modal(lv_obj_t *parent)
{
    s_add_modal = lv_obj_create(parent);
    lv_obj_set_size(s_add_modal, 480, 480);
    lv_obj_set_pos(s_add_modal, 0, 0);
    lv_obj_set_style_bg_color(s_add_modal, col(0x000000), 0);
    lv_obj_set_style_bg_opa(s_add_modal, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_add_modal, 0, 0);
    lv_obj_set_style_radius(s_add_modal, 0, 0);
    lv_obj_set_style_pad_all(s_add_modal, 0, 0);
    lv_obj_add_flag(s_add_modal, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *card = lv_obj_create(s_add_modal);
    lv_obj_set_size(card, 460, 404);
    lv_obj_set_pos(card, 10, 36);
    lv_obj_set_style_bg_color(card, col(0x10254A), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_add_title = ui_label(card, "添加个股（右栏）", NULL, C_TXT);
    lv_obj_align(s_add_title, LV_ALIGN_TOP_LEFT, 4, 0);

    /* 个股 / 指数 二选一 */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(card);
        lv_obj_set_size(b, 210, 36);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 4 + i * 218, 28);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, col(0x16203A), 0);
        lv_obj_add_event_cb(b, add_mode_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, i ? "指数（左栏）" : "个股（右栏）", &ui_cjk_16, 0x6B7F9E);
        lv_obj_center(l);
        s_add_mode_btn[i] = b;
    }

    s_add_hint = ui_label(card, "6 位股票代码，如 600519", &ui_cjk_16, 0x5B6B85);
    lv_obj_align(s_add_hint, LV_ALIGN_TOP_LEFT, 6, 70);

    lv_obj_t *ta = lv_textarea_create(card);
    lv_obj_set_size(ta, 440, 50);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 94);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 8);
    lv_textarea_set_placeholder_text(ta, "代码");
    lv_obj_set_style_text_font(ta, &ui_cjk_16, 0);
    lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_CENTER, 0);
    s_add_ta = ta;

    lv_obj_t *kb = lv_keyboard_create(card);
    lv_obj_set_size(kb, 440, 196);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 150);
    lv_keyboard_set_mode(kb, (s_mkt == MKT_US) ? LV_KEYBOARD_MODE_TEXT_LOWER
                                               : LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, ta);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 130, 40);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 40, 352);
    lv_obj_set_style_bg_color(cancel, col(0x39496B), 0);
    lv_obj_add_event_cb(cancel, add_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = ui_label(cancel, "取消", NULL, C_TXT);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_set_size(ok, 130, 40);
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -40, 352);
    lv_obj_set_style_bg_color(ok, col(C_ACCENT), 0);
    lv_obj_add_event_cb(ok, add_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = ui_label(ok, "添加", NULL, C_TXT);
    lv_obj_center(ol);

    add_mode_refresh();
}

/* 每次重建界面前清空控件指针。
 * App 没开"后台"时退出会销毁整个界面，静态指针就此悬空；
 * 下一次 create 里若还有函数先用到它（比如按旧按钮改样式），
 * 就会 Load access fault 直接重启。 */
static void reset_ui_refs(void)
{
    s_list = NULL;
    s_lbl_empty = NULL;
    s_lbl_count = NULL;
    s_lbl_status = NULL;
    s_lbl_clock = NULL;
    s_lbl_date = NULL;
    s_idx_list = NULL;
    memset(s_idxui, 0, sizeof(s_idxui));
    s_add_modal = NULL;
    s_add_ta = NULL;
    s_add_title = NULL;
    s_add_hint = NULL;
    memset(s_add_mode_btn, 0, sizeof(s_add_mode_btn));
    s_detail = NULL;
    s_alist = NULL;
    s_aedit = NULL;
    s_ae_name = NULL;
    s_ae_now = NULL;
    s_ae_lbl = NULL;
    memset(s_ae_dirl, 0, sizeof(s_ae_dirl));
    s_ae_daylbl = NULL;
    s_ae_slider = NULL;
    s_alist_box = NULL;
    s_alist_empty = NULL;
    s_dt_name = NULL;
    s_dt_code = NULL;
    s_dt_price = NULL;
    s_dt_chg = NULL;
    memset(s_dt_cell, 0, sizeof(s_dt_cell));
    memset(s_dt_key, 0, sizeof(s_dt_key));
    s_dt_alerts = NULL;
    s_dt_alert_btn = NULL;
    memset(s_kp_btn, 0, sizeof(s_kp_btn));
    s_kimg = NULL;
    s_kmsg = NULL;
    s_k_hdr = NULL;
    memset(s_kaxis, 0, sizeof(s_kaxis));
    s_ksel = -1;
    s_kpan = false;
    memset(s_rows, 0, sizeof(s_rows));
}

/* ================= 市场切换（A 股 / 美股） ================= */
static void mkt_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("stock", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "mkt", &v) != ESP_OK) {
            v = 0;
        }
        nvs_close(h);
    }
    s_mkt = (v == 1) ? MKT_US : MKT_A;
}

static void mkt_save(void)
{
    nvs_handle_t h;
    if (nvs_open("stock", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "mkt", (uint8_t)s_mkt);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void mkt_switch_cb(lv_event_t *e)
{
    mkt_t m = (mkt_t)(intptr_t)lv_event_get_user_data(e);
    if (m == s_mkt) {
        return;
    }
    s_mkt = m;
    mkt_save();

    if (s_detail) {
        lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    }
    s_detail_idx = -1;
    s_detail_kind = 0;

    watch_load();
    idx_load();
    rebuild_idx();
    rebuild_list();
    for (int i = 0; i < 2; i++) {
        if (!s_mkt_btn[i]) {
            continue;
        }
        lv_obj_set_style_bg_color(s_mkt_btn[i], col(i == (int)m ? 0x8A3AB2 : 0x16203A), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_mkt_btn[i], 0),
                                    col(i == (int)m ? C_TXT : 0x6B7F9E), 0);
    }
    s_last_poll_ms = 0;
    fetch_now();
    ui_shell_toast(m == MKT_A ? "已切换到 A 股" : "已切换到美股");
    ESP_LOGI(TAG, "market switched to %s", m == MKT_A ? "A" : "US");
}

static void create(lv_obj_t *parent)
{
    mkt_load();
    reset_ui_refs();
    lv_obj_set_style_bg_color(parent, col(0x05101F), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    s_detail_idx = -1;
    s_alist_filter[0] = '\0';

    watch_load();
    idx_load();
    alert_load();

    /* ---------- 左栏：交易状态 / 时间 / 日期星期（三行分开） ---------- */
    /* 注意：open_app_idx 会把 y>=34 的内容整体上移 34px（app_shift_up），
     * 所以这里写 40，实际显示在 y=6；指数卡片同理写 146 显示在 112。 */
    lv_obj_t *clk = ui_card(parent, 164, 96, C_BG_CARD);
    lv_obj_set_pos(clk, 6, 40);
    lv_obj_set_style_pad_all(clk, 2, 0);

    s_lbl_status = lv_label_create(clk);
    lv_label_set_text(s_lbl_status, "--");
    lv_obj_set_style_text_font(s_lbl_status, &ui_cjk_16, 0);
    lv_obj_set_style_text_color(s_lbl_status, col(0x8FA8CC), 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_LEFT, 2, 3);

    /* 右上角：A股 / 美股 切换 */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(clk);
        lv_obj_set_size(b, 36, 22);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, i ? -1 : -39, 1);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_bg_color(b, col(i == (int)s_mkt ? 0x8A3AB2 : 0x16203A), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(b, mkt_switch_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(b, i ? "美股" : "A股", &ui_cjk_16,
                               i == (int)s_mkt ? C_TXT : 0x6B7F9E);
        lv_obj_center(l);
        s_mkt_btn[i] = b;
    }

    s_lbl_clock = lv_label_create(clk);
    lv_label_set_text(s_lbl_clock, "--:--:--");
    lv_obj_set_style_text_font(s_lbl_clock, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_clock, col(C_TXT), 0);
    lv_obj_align(s_lbl_clock, LV_ALIGN_TOP_MID, 0, 24);

    /* 时间下面：日期 + 星期 */
    s_lbl_date = ui_label(clk, "--/--  ---", &ui_cjk_16, C_TXT_DIM);
    lv_obj_align(s_lbl_date, LV_ALIGN_TOP_MID, 0, 62);

    /* ---------- 左栏：指数（可增删 / 可拖动排序，多了能滚） ---------- */
    s_idx_list = lv_obj_create(parent);
    lv_obj_set_size(s_idx_list, 164, 334);
    lv_obj_set_pos(s_idx_list, 6, 146);
    lv_obj_set_style_bg_opa(s_idx_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_idx_list, 0, 0);
    lv_obj_set_style_radius(s_idx_list, 0, 0);
    lv_obj_set_style_pad_all(s_idx_list, 0, 0);
    lv_obj_set_scroll_dir(s_idx_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_idx_list, LV_SCROLLBAR_MODE_AUTO);
    rebuild_idx();

    /* ---------- 右栏：自选股（顶到边） ---------- */
    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, 294, 474);
    lv_obj_set_pos(s_list, 178, 6);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_bottom(s_list, 46, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    s_lbl_empty = ui_label(parent, "点右下角 + 添加自选股", NULL, C_TXT_DIM);
    lv_obj_set_style_text_align(s_lbl_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_lbl_empty, LV_ALIGN_TOP_LEFT, 190, 200);

    /* 添加按钮：右下角悬浮圆钮（不占列表版面） */
    lv_obj_t *add = lv_button_create(parent);
    lv_obj_set_size(add, 46, 46);
    lv_obj_set_pos(add, 480 - 58, 480 - 58);
    lv_obj_set_style_radius(add, 23, 0);
    lv_obj_set_style_bg_color(add, col(C_ACCENT), 0);
    lv_obj_set_style_shadow_width(add, 16, 0);
    lv_obj_set_style_shadow_color(add, col(0x000000), 0);
    lv_obj_set_style_shadow_opa(add, LV_OPA_40, 0);
    lv_obj_add_event_cb(add, add_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = ui_label(add, "+", &lv_font_montserrat_28, C_TXT);
    lv_obj_center(al);

    lv_obj_t *bell = lv_button_create(parent);
    lv_obj_set_size(bell, 46, 46);
    lv_obj_set_pos(bell, 480 - 58, 480 - 58 - 54);
    lv_obj_set_style_radius(bell, 23, 0);
    lv_obj_set_style_bg_color(bell, col(0x8A3AB2), 0);
    lv_obj_add_event_cb(bell, alist_open_all_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bll = lv_label_create(bell);
    lv_label_set_text(bll, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(bll, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bll, col(C_TXT), 0);
    lv_obj_center(bll);

    build_add_modal(parent);
    build_detail(parent);
    build_alert_list(parent);
    build_alert_edit(parent);
    rebuild_list();

    if (!s_sem) {
        /* R-36：任务创建失败时要明确报错，不能留下"信号量在、没人消费"的半死状态。
         * 注意顺序：任务起来就会 xSemaphoreTake(s_sem)，必须先赋值再建任务，
         * 否则 take(NULL) 会 assert 重启。 */
        s_sem = xSemaphoreCreateBinary();
        if (s_sem) {
            BaseType_t ok = xTaskCreate(fetch_task, "stock_fetch", 8192, NULL, 3, NULL);
            if (ok != pdPASS) {
                ESP_LOGW(TAG, "stock_fetch 8192 failed, retry 6144");
                ok = xTaskCreate(fetch_task, "stock_fetch", 6144, NULL, 3, NULL);
            }
            if (ok == pdPASS) {
                ESP_LOGI(TAG, "stock_fetch task started");
            } else {
                vSemaphoreDelete(s_sem);
                s_sem = NULL;
                ESP_LOGE(TAG, "stock_fetch create failed: 行情不可用");
                ui_shell_toast("内存不足：行情暂不可用");
            }
        }
    }

    /* 行情轮询：1 秒心跳，具体拉不拉由 stock_poll_cb 按刷新率设置决定 */
    static bool timer_started;
    if (!timer_started) {
        timer_started = true;
        const esp_timer_create_args_t args = {
            .callback = stock_poll_cb,
            .name = "stk_poll",
        };
        esp_timer_handle_t th = NULL;
        if (esp_timer_create(&args, &th) == ESP_OK) {
            esp_timer_start_periodic(th, 1000 * 1000);
        }
    }

    /* K 线抓取任务提前建好：别等用户点进详情页才建任务（那一下最容易出问题） */
    kline_task_start();
}

static void tick(void)
{
    /* 交易状态 + 时分秒 + 日期（每秒刷新） */
    static int sec;
    if (++sec >= 2) {          /* tick 约 0.5s */
        sec = 0;
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        if (t.tm_year + 1900 >= 2026) {
            uint32_t color = 0x8FA8CC;
            const char *st = trade_status(&t, &color);
            lv_label_set_text(s_lbl_status, st);
            lv_obj_set_style_text_color(s_lbl_status, col(color), 0);

            static const char *wd[] = { "周日", "周一", "周二", "周三",
                                        "周四", "周五", "周六" };
            if (s_mkt == MKT_US) {
                struct tm et;
                to_us_east(now, &et);
                lv_label_set_text_fmt(s_lbl_clock, "%02d:%02d:%02d",
                                      et.tm_hour, et.tm_min, et.tm_sec);
                lv_label_set_text_fmt(s_lbl_date, "%02d-%02d %s · 美东",
                                      et.tm_mon + 1, et.tm_mday, wd[et.tm_wday]);
            } else {
                lv_label_set_text_fmt(s_lbl_clock, "%02d:%02d:%02d",
                                      t.tm_hour, t.tm_min, t.tm_sec);
                lv_label_set_text_fmt(s_lbl_date, "%02d-%02d %s",
                                      t.tm_mon + 1, t.tm_mday, wd[t.tm_wday]);
            }
        }
    }

    if (s_dirty) {
        s_dirty = false;
        apply_data();
    }

    /* 提醒增删/命中：抓取任务只置标志，这里在 LVGL 线程里刷 */
    if (s_alert_ui_dirty) {
        s_alert_ui_dirty = false;
        if (s_alist && !lv_obj_has_flag(s_alist, LV_OBJ_FLAG_HIDDEN)) {
            alist_rebuild();
        }
        if (s_detail && !lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN)) {
            detail_refresh();
        }
    }

    /* 兜底：抓取任务如果异常卡住，25 秒后放开，允许再次请求 */
    if (s_kline_busy && s_kline_req_ms > 0 &&
        esp_timer_get_time() / 1000 - s_kline_req_ms > 25000) {
        s_kline_busy = false;
        s_kline_fail = true;
        s_kline_dirty = true;
    }

    /* K 线数据是后台任务拉的，回到 LVGL 线程再画 */
    if (s_kline_dirty) {
        s_kline_dirty = false;
        if (s_detail && !lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN)) {
            kline_redraw();
        }
    }

    /* 行情刷新不在这里节流：由 stock_poll_cb（1 秒心跳）按设置里的
     * 前台刷新率决定，1 秒档才真的是每秒一次。 */
}

static void on_show(void)
{
    s_visible = true;
    ui_shell_set_tick_cb(tick);
    stock_cfg_load();
    s_last_poll_ms = 0;          /* 进 App 立刻拉一次 */
    fetch_now();
    quote_t *dq = detail_quote();
    if (dq) {
        kline_request(dq->ksym[0] ? dq->ksym : dq->secid);
    }
}

static void on_hide(void)
{
    s_visible = false;
    ui_shell_set_tick_cb(NULL);
}


/* 调试钩子：发布版必须为 0（R-13/G9），只有需要自动压测时才临时改 1 */
#define STK_DEBUG_API 0

#if STK_DEBUG_API
/* 调试用（main.c 的 STK_SELFTEST 任务调用）：必须在 lvgl_port_lock 内 */
void ui_stock_debug_open(int idx)
{
    open_detail(idx);
}

void ui_stock_debug_close(void)
{
    if (s_detail) {
        lv_obj_add_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    }
    s_detail_idx = -1;
    s_ksel = -1;
}

void ui_stock_debug_report(void)
{
    ESP_LOGI(TAG, "== report mkt=%d idx_cnt=%d watch_cnt=%d",
             (int)s_mkt, s_idx_cnt, s_watch_cnt);
    for (int i = 0; i < s_idx_cnt; i++) {
        ESP_LOGI(TAG, "   idx%d %-8s %-10s price=%.2f pct=%+.2f valid=%d ksym=%s",
                 i, s_idx[i].secid, s_idx[i].name, (double)s_idx[i].price,
                 (double)s_idx[i].pct, (int)s_idx[i].valid, s_idx[i].ksym);
    }
    for (int i = 0; i < s_watch_cnt; i++) {
        ESP_LOGI(TAG, "   stk%d %-8s %-10s price=%.2f pct=%+.2f valid=%d ksym=%s",
                 i, s_watch[i].secid, s_watch[i].name, (double)s_watch[i].price,
                 (double)s_watch[i].pct, (int)s_watch[i].valid, s_watch[i].ksym);
    }
}

void ui_stock_debug_market(int m)
{
    mkt_t want = (m == 1) ? MKT_US : MKT_A;
    if (want == s_mkt) {
        return;
    }
    /* 复用切换逻辑（不经过按钮回调） */
    s_mkt = want;
    mkt_save();
    watch_load();
    idx_load();
    rebuild_idx();
    rebuild_list();
    s_last_poll_ms = 0;
    fetch_now();
    ESP_LOGI(TAG, "debug market -> %d", m);
}

void ui_stock_debug_period(int p)
{
    s_kperiod = (p < 0 || p > 3) ? KP_DAY : p;
    ESP_LOGI(TAG, "debug period -> %s", kp_name());
    quote_t *q = detail_quote();
    if (q) {
        kline_request_ex(q->ksym[0] ? q->ksym : q->secid, true);
    }
}

void ui_stock_debug_open_idx(int i)
{
    open_idx_detail(i);
}

void ui_stock_debug_open_stk(int i)
{
    open_detail(i);
}

void ui_stock_debug_report_k(void)
{
    ESP_LOGI(TAG, "   kline %s n=%d period=%s fail=%d busy=%d first=%s last=%s",
             s_kbar_secid, s_kbar_n, kp_name(), (int)s_kline_fail,
             (int)s_kline_busy,
             s_kbar_n ? s_kbar[0].d : "-",
             s_kbar_n ? s_kbar[s_kbar_n - 1].d : "-");
    if (s_kbar_n > 0) {
        kbar_t *k = &s_kbar[s_kbar_n - 1];
        ESP_LOGI(TAG, "   last O%.2f H%.2f L%.2f C%.2f",
                 (double)k->o, (double)k->h, (double)k->l, (double)k->c);
    }
}
#endif  /* STK_DEBUG_API */

const ui_app_t ui_app_stock = {
    .name = "交易",
    .symbol = LV_SYMBOL_BARS,
    .color = 0x8A3AB2,
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
