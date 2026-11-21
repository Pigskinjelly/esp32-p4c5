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

static const char *TAG = "ui_stock";

#define MAX_WATCH   8
#define IDX_COUNT   4

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
    bool  valid;
} quote_t;

/* 大盘指数（固定，不可删） */
static const char *s_idx_secid[IDX_COUNT] = {
    "sh000001", "sz399001", "sz399006", "sh000688",
};
static const char *s_idx_name[IDX_COUNT] = {
    "上证", "深证", "创业", "科创",
};

static quote_t s_idx[IDX_COUNT];
static quote_t s_watch[MAX_WATCH];
static int s_watch_cnt;

static volatile bool s_dirty;
static volatile bool s_busy;
static SemaphoreHandle_t s_sem;

/* 前置声明（这些函数在后面定义） */
static volatile bool s_visible;
static void alert_check(void);
static void alert_drop_stock(const char *secid);
static void alert_load(void);
static void row_click_cb(lv_event_t *e);
static void fetch_now(void);
static void open_detail(int idx);
static void detail_refresh(void);
static void alist_rebuild(void);
static void alist_open(const char *secid);
static void alist_price_refresh(void);
static lv_obj_t *s_alist;

/* ================= 交易状态（A 股交易时段） ================= */

/*
 * 09:15-09:30 竞价 / 09:30-11:30 交易中 / 11:30-13:00 午休
 * 13:00-14:57 交易中 / 14:57-15:00 竞价 / 15:00-15:30 盘后
 * 其余（含周末）已收盘
 */
static const char *trade_status(struct tm *t, uint32_t *color)
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

/* ================= 自选股持久化 ================= */

static void watch_save(void)
{
    char buf[512] = { 0 };
    size_t off = 0;
    for (int i = 0; i < s_watch_cnt && off < sizeof(buf) - 48; i++) {
        off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s%s|%s",
                                i ? ";" : "", s_watch[i].secid, s_watch[i].name);
    }
    nvs_handle_t h;
    if (nvs_open("stock", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "list", buf);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "watchlist saved: %s", buf);
}

static void watch_load(void)
{
    char buf[512] = { 0 };
    size_t len = sizeof(buf);
    nvs_handle_t h;
    s_watch_cnt = 0;
    if (nvs_open("stock", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "list", buf, &len);
        nvs_close(h);
    }
    if (buf[0] == '\0') {
        const char *def[3][2] = {
            { "sh600519", "贵州茅台" },
            { "sz000001", "平安银行" },
            { "sz002517", "恺英网络" },
        };
        for (int i = 0; i < 3; i++) {
            strlcpy(s_watch[i].secid, def[i][0], sizeof(s_watch[i].secid));
            strlcpy(s_watch[i].name, def[i][1], sizeof(s_watch[i].name));
            strlcpy(s_watch[i].code, def[i][0] + 2, sizeof(s_watch[i].code));
            s_watch_cnt++;
        }
        watch_save();
        return;
    }
    char *save = NULL;
    char *tok = strtok_r(buf, ";", &save);
    while (tok && s_watch_cnt < MAX_WATCH) {
        char *bar = strchr(tok, '|');
        quote_t *w = &s_watch[s_watch_cnt];
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
        s_watch_cnt++;
        tok = strtok_r(NULL, ";", &save);
    }
    ESP_LOGI(TAG, "watchlist loaded: %d stock(s)", s_watch_cnt);
}

static void code_to_secid(const char *code, char *secid, size_t sz)
{
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

    /* 指数 */
    for (int i = 0; i < IDX_COUNT; i++) {
        if (strcmp(secid, s_idx_secid[i]) == 0) {
            s_idx[i].price = (float)atof(f[3]);
            s_idx[i].chg = (float)atof(f[31]);
            s_idx[i].pct = (float)atof(f[32]);
            s_idx[i].valid = true;
            return;
        }
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
        for (int i = 0; i < IDX_COUNT; i++) {
            off += (size_t)snprintf(url + off, sizeof(url) - off, "%s,", s_idx_secid[i]);
        }
        for (int i = 0; i < s_watch_cnt; i++) {
            off += (size_t)snprintf(url + off, sizeof(url) - off, "%s%s",
                                    i ? "," : "", s_watch[i].secid);
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

/* 左栏指数 */
static lv_obj_t *s_idx_val[IDX_COUNT];
static lv_obj_t *s_idx_pct[IDX_COUNT];

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

static void rebuild_list(void);

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
        /* 点卡片本身 → 设置价格提醒 */
        lv_obj_add_event_cb(card, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
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
    for (int i = 0; i < IDX_COUNT; i++) {
        if (!s_idx[i].valid || !s_idx_val[i]) {
            continue;
        }
        lv_label_set_text_fmt(s_idx_val[i], "%.2f", s_idx[i].price);
        lv_label_set_text_fmt(s_idx_pct[i], "%+.2f%%", s_idx[i].pct);
        lv_obj_set_style_text_color(s_idx_pct[i], col(pct_color(s_idx[i].pct)), 0);
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
        if (s_alist && !lv_obj_has_flag(s_alist, LV_OBJ_FLAG_HIDDEN)) {
            alist_rebuild();
        }
        if (s_detail && !lv_obj_has_flag(s_detail, LV_OBJ_FLAG_HIDDEN)) {
            detail_refresh();
        }
    }
}

/* 后台轮询：只有「交易」开了"后台"且有未触发的提醒时才拉行情 */
static void alert_poll_cb(void *arg)
{
    (void)arg;
    if (!ui_shell_app_bg_by_name("交易")) {
        return;
    }
    if (s_alert_cnt == 0 || wifi_mgr_get_state() != WIFI_MGR_CONNECTED) {
        return;
    }
    fetch_now();
}

/* ================= 个股详情 + 日K ================= */

#define KLINE_N     60
#define KCH_W       444
#define KCH_H       120

typedef struct {
    float o, c, h, l;
} kbar_t;

static kbar_t s_kbar[KLINE_N];
static int    s_kbar_n;
static char   s_kbar_secid[12];
static int64_t s_kbar_ms;          /* 上次抓取时间 */
static char   s_kline_req[12];
static volatile bool s_kline_busy;
static int64_t s_kline_req_ms;
static volatile bool s_kline_dirty;
static volatile bool s_kline_fail;   /* 两个源都没拿到 */
static SemaphoreHandle_t s_kline_sem;
static lv_obj_t *s_kimg;         /* K 线画布（一张图，不再一个柱子一个对象） */
static lv_obj_t *s_kmsg;         /* 加载中/失败提示 */
static lv_obj_t *s_k_hdr;
static uint16_t *s_kbuf;         /* PSRAM 画布 */
static lv_image_dsc_t s_kdsc;

static void detail_close_cb(lv_event_t *e)
{
    (void)e;
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

/* 画布：444x120 RGB565，全部手工填像素（对象只有一个 lv_image） */
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

static void kline_redraw(void)
{
    if (!s_kimg) {
        return;
    }

    if (s_kbar_n < 2) {
        lv_obj_add_flag(s_kimg, LV_OBJ_FLAG_HIDDEN);
        if (s_kmsg) {
            lv_label_set_text(s_kmsg, s_kline_fail ? "日K 获取失败，点此重试" : "日K 加载中…");
            lv_obj_set_style_text_color(s_kmsg, col(s_kline_fail ? 0xFF8A8A : 0x5B6B85), 0);
            lv_obj_remove_flag(s_kmsg, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_k_hdr) {
            lv_label_set_text(s_k_hdr, "日K");
        }
        return;
    }

    float hi = s_kbar[0].h;
    float lo = s_kbar[0].l;
    for (int i = 1; i < s_kbar_n; i++) {
        if (s_kbar[i].h > hi) hi = s_kbar[i].h;
        if (s_kbar[i].l < lo) lo = s_kbar[i].l;
    }
    float span = hi - lo;
    if (span < 0.001f) {
        span = hi * 0.01f + 0.01f;
    }

    uint16_t bg = kpix(C_BG_CARD);
    kfill(0, 0, KCH_W, KCH_H, bg);

    int n = s_kbar_n;
    int slot = KCH_W / n;
    if (slot < 3) {
        slot = 3;
    }
    int bw = slot - 2;
    if (bw < 2) {
        bw = 2;
    }

    for (int i = 0; i < n; i++) {
        int x = i * slot;
        int yh = KCH_H - 2 - (int)((s_kbar[i].h - lo) * (KCH_H - 4) / span);
        int yl = KCH_H - 2 - (int)((s_kbar[i].l - lo) * (KCH_H - 4) / span);
        int yo = KCH_H - 2 - (int)((s_kbar[i].o - lo) * (KCH_H - 4) / span);
        int yc = KCH_H - 2 - (int)((s_kbar[i].c - lo) * (KCH_H - 4) / span);
        uint16_t cr = kpix((s_kbar[i].c >= s_kbar[i].o) ? C_UP : C_DOWN);

        kfill(x + bw / 2, yh, 1, yl - yh > 0 ? yl - yh : 1, cr);      /* 影线 */
        int top = (yo < yc) ? yo : yc;
        int hgt = (yo > yc ? yo - yc : yc - yo);
        kfill(x, top, bw, hgt < 2 ? 2 : hgt, cr);                     /* 实体 */
    }

    lv_obj_remove_flag(s_kimg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_kimg);
    if (s_kmsg) {
        lv_obj_add_flag(s_kmsg, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_k_hdr) {
        int dec = 2;
        if (s_detail_idx >= 0 && s_detail_idx < s_watch_cnt && s_watch[s_detail_idx].dec >= 2) {
            dec = s_watch[s_detail_idx].dec;
        }
        lv_label_set_text_fmt(s_k_hdr, "日K %d日   高 %.*f  低 %.*f", n, dec, hi, dec, lo);
    }
}

/* 从腾讯 K 线接口里取出 ["日期",开,收,高,低,量] */
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

static int kline_parse(const char *body, kbar_t *out, int max)
{
    const char *p = strstr(body, "\"qfqday\":[[");
    if (!p) {
        p = strstr(body, "\"day\":[[");
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
        char f[6][20];
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
            n++;
        }
        q = r;
    }
    return n;
}

/* 新浪备用源：[{"day":"...","open":"..","high":"..","low":"..","close":".."}] */
static int kline_parse_sina(const char *body, kbar_t *out, int max)
{
    int n = 0;
    const char *p = body;
    while (n < max) {
        const char *d = strstr(p, "\"day\":\"");
        if (!d) {
            break;
        }
        const char *o = strstr(d, "\"open\":\"");
        const char *h = strstr(d, "\"high\":\"");
        const char *l = strstr(d, "\"low\":\"");
        const char *c = strstr(d, "\"close\":\"");
        if (!o || !h || !l || !c) {
            break;
        }
        out[n].o = (float)atof(o + 8);
        out[n].h = (float)atof(h + 8);
        out[n].l = (float)atof(l + 8);
        out[n].c = (float)atof(c + 9);
        n++;
        p = c + 9;
    }
    return n;
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
    static kbar_t tmp[90];
    while (1) {
        if (xSemaphoreTake(s_kline_sem, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (wifi_mgr_get_state() != WIFI_MGR_CONNECTED) {
            s_kline_busy = false;
            continue;
        }
        static char body[16384];
        char url[260];
        int got = 0;

        /* 按"实测可用性"排序：腾讯 https 通常最快；失败就换新浪（http 在国内最稳） */
        typedef struct {
            const char *url;
            bool sina;
        } ksrc_t;
        static const ksrc_t srcs[4] = {
            { "https://web.ifzq.gtimg.cn/appstock/app/fqkline/get?param=%s,day,,,60,qfq", false },
            { "http://money.finance.sina.com.cn/quotes_service/api/json_v2.php/"
              "CN_MarketData.getKLineData?symbol=%s&scale=240&ma=no&datalen=60", true },
            { "https://proxy.finance.qq.com/ifzqgtimg/appstock/app/fqkline/get?param=%s,day,,,60,qfq", false },
            { "https://money.finance.sina.com.cn/quotes_service/api/json_v2.php/"
              "CN_MarketData.getKLineData?symbol=%s&scale=240&ma=no&datalen=60", true },
        };
        int total = 0;
        for (int i = 0; i < 4 && got < 2; i++) {
            snprintf(url, sizeof(url), srcs[i].url, s_kline_req);
            total = kline_http_get(url, body, sizeof(body));
            if (total > 0) {
                got = srcs[i].sina ? kline_parse_sina(body, tmp, 90)
                                   : kline_parse(body, tmp, 90);
            }
            ESP_LOGI(TAG, "kline src#%d %s: bytes=%d bars=%d",
                     i + 1, s_kline_req, total, got);
            if (got < 2) {
                vTaskDelay(pdMS_TO_TICKS(300));
            }
        }
        if (got > KLINE_N) {
            memmove(tmp, tmp + (got - KLINE_N), sizeof(kbar_t) * (size_t)KLINE_N);
            got = KLINE_N;
        }
        if (got >= 2) {
            memcpy(s_kbar, tmp, sizeof(kbar_t) * (size_t)got);
            s_kbar_n = got;
            strlcpy(s_kbar_secid, s_kline_req, sizeof(s_kbar_secid));
            s_kbar_ms = esp_timer_get_time() / 1000;
            s_kline_fail = false;
        } else {
            s_kline_fail = true;
        }
        s_kline_dirty = true;
        s_kline_busy = false;
    }
}

static void kline_request_ex(const char *secid, bool force)
{
    if (!s_kline_sem) {
        s_kline_sem = xSemaphoreCreateBinary();
        if (s_kline_sem) {
            BaseType_t ok = xTaskCreateWithCaps(kline_fetch_task, "kline", 10240,
                                                NULL, 2, NULL, MALLOC_CAP_SPIRAM);
            ESP_LOGI(TAG, "kline task %s", ok == pdPASS ? "started" : "FAILED");
        }
    }
    bool fresh = (!force && strcmp(s_kbar_secid, secid) == 0 && s_kbar_n >= 2 &&
                  esp_timer_get_time() / 1000 - s_kbar_ms < 5 * 60 * 1000);
    if (fresh) {
        kline_redraw();
        return;
    }
    if (s_kline_busy) {
        return;
    }
    s_kbar_n = 0;
    s_kbar_secid[0] = '\0';
    s_kline_fail = false;
    strlcpy(s_kline_req, secid, sizeof(s_kline_req));
    s_kline_busy = true;
    s_kline_req_ms = esp_timer_get_time() / 1000;
    s_kline_dirty = true;
    ESP_LOGI(TAG, "kline request %s", secid);
    if (s_kline_sem) {
        xSemaphoreGive(s_kline_sem);
    }
    kline_redraw();
}

static void kline_request(const char *secid)
{
    kline_request_ex(secid, false);
}

/* 图表区域点一下：失败时重试 */
static void kline_retry_cb(lv_event_t *e)
{
    (void)e;
    if (!s_kline_fail || s_detail_idx < 0 || s_detail_idx >= s_watch_cnt) {
        return;
    }
    kline_request_ex(s_watch[s_detail_idx].secid, true);
    kline_redraw();
}

/* ---------------- 详情页 ---------------- */

static void detail_refresh(void)
{
    if (!s_detail || s_detail_idx < 0 || s_detail_idx >= s_watch_cnt) {
        return;
    }
    quote_t *q = &s_watch[s_detail_idx];
    if (!q->valid) {
        return;
    }
    int dec = (q->dec >= 2 && q->dec <= 4) ? q->dec : 2;
    char b[48];

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
    if (q->vol >= 10000.0f) {
        snprintf(b, sizeof(b), "%.2f 万手", q->vol / 10000.0f);
    } else {
        snprintf(b, sizeof(b), "%.0f 手", q->vol);
    }
    lv_label_set_text(s_dt_cell[4], b);
    if (q->amt >= 10000.0f) {
        snprintf(b, sizeof(b), "%.2f 亿", q->amt / 10000.0f);
    } else {
        snprintf(b, sizeof(b), "%.0f 万", q->amt);
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
    lv_obj_set_size(back, 66, 30);
    lv_obj_set_pos(back, 10, 6);
    lv_obj_set_style_radius(back, 15, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_set_style_bg_color(back, col(0x16203A), 0);
    lv_obj_add_event_cb(back, detail_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(back, "返回", &ui_cjk_16, C_TXT);
    lv_obj_center(bl);

    s_dt_name = ui_label(s_detail, "-", &ui_cjk_24, C_TXT);
    lv_obj_set_pos(s_dt_name, 88, 2);
    s_dt_code = ui_label(s_detail, "", NULL, C_TXT_DIM);
    lv_obj_align(s_dt_code, LV_ALIGN_TOP_RIGHT, -14, 10);

    s_dt_price = lv_label_create(s_detail);
    lv_label_set_text(s_dt_price, "----");
    lv_obj_set_style_text_font(s_dt_price, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_dt_price, col(C_TXT), 0);
    lv_obj_set_pos(s_dt_price, 14, 36);

    s_dt_chg = ui_label(s_detail, "--", &ui_cjk_16, C_TXT_DIM);
    lv_obj_set_pos(s_dt_chg, 16, 76);

    static const char *k1[3] = { "今开", "最高", "成交量" };
    static const char *k2[3] = { "昨收", "最低", "成交额" };
    for (int i = 0; i < 3; i++) {
        int y = 102 + i * 38;
        lv_obj_t *c1 = ui_card(s_detail, 218, 34, C_BG_CARD);
        lv_obj_set_pos(c1, 12, y);
        lv_obj_t *l1 = ui_label(c1, k1[i], NULL, C_TXT_DIM);
        lv_obj_align(l1, LV_ALIGN_LEFT_MID, 4, 0);
        s_dt_cell[i * 2] = ui_label(c1, "--", NULL, C_TXT);
        lv_obj_align(s_dt_cell[i * 2], LV_ALIGN_RIGHT_MID, -6, 0);

        lv_obj_t *c2 = ui_card(s_detail, 218, 34, C_BG_CARD);
        lv_obj_set_pos(c2, 250, y);
        lv_obj_t *l2 = ui_label(c2, k2[i], NULL, C_TXT_DIM);
        lv_obj_align(l2, LV_ALIGN_LEFT_MID, 4, 0);
        s_dt_cell[i * 2 + 1] = ui_label(c2, "--", NULL, C_TXT);
        lv_obj_align(s_dt_cell[i * 2 + 1], LV_ALIGN_RIGHT_MID, -6, 0);
    }

    /* ---- 日K ---- */
    lv_obj_t *kc = ui_card(s_detail, 456, 156, C_BG_CARD);
    lv_obj_set_pos(kc, 12, 218);
    lv_obj_set_style_pad_all(kc, 0, 0);

    s_k_hdr = ui_label(kc, "日K", NULL, C_TXT_DIM);
    lv_obj_set_pos(s_k_hdr, 8, 4);

    if (!s_kbuf) {
        s_kbuf = heap_caps_malloc(KCH_W * KCH_H * 2, MALLOC_CAP_SPIRAM);
    }
    if (s_kbuf) {
        memset(s_kbuf, 0, KCH_W * KCH_H * 2);
        memset(&s_kdsc, 0, sizeof(s_kdsc));
        s_kdsc.header.cf = LV_COLOR_FORMAT_RGB565;
        s_kdsc.header.w = KCH_W;
        s_kdsc.header.h = KCH_H;
        s_kdsc.header.stride = KCH_W * 2;
        s_kdsc.data = (const uint8_t *)s_kbuf;
        s_kdsc.data_size = KCH_W * KCH_H * 2;
    }
    s_kimg = lv_image_create(kc);
    lv_obj_set_pos(s_kimg, 6, 26);
    if (s_kbuf) {
        lv_image_set_src(s_kimg, &s_kdsc);
    }
    lv_obj_add_flag(s_kimg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_kimg, kline_retry_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_kimg, LV_OBJ_FLAG_HIDDEN);

    s_kmsg = ui_label(kc, "日K 加载中…", NULL, 0x5B6B85);
    lv_obj_align(s_kmsg, LV_ALIGN_CENTER, 0, 10);
    lv_obj_add_flag(s_kmsg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_kmsg, kline_retry_cb, LV_EVENT_CLICKED, NULL);

    s_dt_alerts = ui_label(s_detail, "", NULL, 0x5B6B85);
    lv_obj_set_pos(s_dt_alerts, 14, 378);

    lv_obj_t *ab = lv_button_create(s_detail);
    lv_obj_set_size(ab, 456, 44);
    lv_obj_set_pos(ab, 12, 400);
    lv_obj_set_style_radius(ab, 12, 0);
    lv_obj_set_style_bg_color(ab, col(0x8A3AB2), 0);
    lv_obj_add_event_cb(ab, detail_alert_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *abl = ui_label(ab, "价格提醒（可设多条）", &ui_cjk_16, C_TXT);
    lv_obj_center(abl);
}

static void open_detail(int idx)
{
    if (!s_detail || idx < 0 || idx >= s_watch_cnt) {
        return;
    }
    s_detail_idx = idx;
    lv_label_set_text(s_dt_name, s_watch[idx].name[0] ? s_watch[idx].name : s_watch[idx].code);
    lv_label_set_text(s_dt_code, s_watch[idx].code);
    detail_refresh();
    kline_request(s_watch[idx].secid);
    lv_obj_remove_flag(s_detail, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_detail);
}

static void row_click_cb(lv_event_t *e)
{
    open_detail((int)(intptr_t)lv_event_get_user_data(e));
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

static void add_close_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_add_modal, LV_OBJ_FLAG_HIDDEN);
}

static void add_ok_cb(lv_event_t *e)
{
    (void)e;
    const char *code = lv_textarea_get_text(s_add_ta);
    if (strlen(code) != 6) {
        ui_shell_toast("请输入 6 位股票代码");
        return;
    }
    char secid[12];
    code_to_secid(code, secid, sizeof(secid));
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
    if (s_watch_cnt >= MAX_WATCH) {
        ui_shell_toast("最多 8 只，请先删除");
        return;
    }
    lv_textarea_set_text(s_add_ta, "");
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
    lv_obj_set_size(card, 460, 400);
    lv_obj_set_pos(card, 10, 40);
    lv_obj_set_style_bg_color(card, col(0x10254A), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = ui_label(card, "添加自选股（输入 6 位代码）", NULL, C_TXT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 0);

    lv_obj_t *ta = lv_textarea_create(card);
    lv_obj_set_size(ta, 440, 54);
    lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 28);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 6);
    lv_textarea_set_placeholder_text(ta, "如 600519");
    lv_obj_set_style_text_font(ta, &ui_cjk_16, 0);
    lv_obj_set_style_text_align(ta, LV_TEXT_ALIGN_CENTER, 0);
    s_add_ta = ta;

    lv_obj_t *kb = lv_keyboard_create(card);
    lv_obj_set_size(kb, 440, 200);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 90);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_NUMBER);
    lv_keyboard_set_textarea(kb, ta);

    lv_obj_t *cancel = lv_button_create(card);
    lv_obj_set_size(cancel, 130, 42);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 40, 300);
    lv_obj_set_style_bg_color(cancel, col(0x39496B), 0);
    lv_obj_add_event_cb(cancel, add_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = ui_label(cancel, "取消", NULL, C_TXT);
    lv_obj_center(cl);

    lv_obj_t *ok = lv_button_create(card);
    lv_obj_set_size(ok, 130, 42);
    lv_obj_align(ok, LV_ALIGN_TOP_RIGHT, -40, 300);
    lv_obj_set_style_bg_color(ok, col(C_ACCENT), 0);
    lv_obj_add_event_cb(ok, add_ok_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ol = ui_label(ok, "添加", NULL, C_TXT);
    lv_obj_center(ol);
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
    memset(s_idx_val, 0, sizeof(s_idx_val));
    memset(s_idx_pct, 0, sizeof(s_idx_pct));
    s_add_modal = NULL;
    s_add_ta = NULL;
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
    s_dt_alerts = NULL;
    s_kimg = NULL;
    s_kmsg = NULL;
    s_k_hdr = NULL;
    memset(s_rows, 0, sizeof(s_rows));
}

static void create(lv_obj_t *parent)
{
    reset_ui_refs();
    lv_obj_set_style_bg_color(parent, col(0x05101F), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    s_detail_idx = -1;
    s_alist_filter[0] = '\0';

    watch_load();
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
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_MID, 0, 2);

    s_lbl_clock = lv_label_create(clk);
    lv_label_set_text(s_lbl_clock, "--:--:--");
    lv_obj_set_style_text_font(s_lbl_clock, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_lbl_clock, col(C_TXT), 0);
    lv_obj_align(s_lbl_clock, LV_ALIGN_TOP_MID, 0, 24);

    /* 时间下面：日期 + 星期 */
    s_lbl_date = ui_label(clk, "--/--  ---", &ui_cjk_16, C_TXT_DIM);
    lv_obj_align(s_lbl_date, LV_ALIGN_TOP_MID, 0, 62);

    /* ---------- 左栏：4 个指数（与时间卡片留出间距） ---------- */
    for (int i = 0; i < IDX_COUNT; i++) {
        lv_obj_t *card = ui_card(parent, 164, 78, C_BG_CARD);
        lv_obj_set_pos(card, 6, 146 + i * 84);
        lv_obj_set_style_pad_all(card, 8, 0);

        lv_obj_t *nm = ui_label(card, s_idx_name[i], NULL, C_TXT_DIM);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 0);

        s_idx_val[i] = lv_label_create(card);
        lv_label_set_text(s_idx_val[i], "----");
        lv_obj_set_style_text_font(s_idx_val[i], &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(s_idx_val[i], col(C_TXT), 0);
        lv_obj_align(s_idx_val[i], LV_ALIGN_TOP_RIGHT, 0, -4);

        s_idx_pct[i] = ui_label(card, "--", NULL, C_DOWN);
        lv_obj_align(s_idx_pct[i], LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }

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
        s_sem = xSemaphoreCreateBinary();
        xTaskCreate(fetch_task, "stock_fetch", 8192, NULL, 3, NULL);
    }

    /* 后台监控：只有「交易」开了"后台运行"才轮询，30 秒一次 */
    static bool timer_started;
    if (!timer_started) {
        timer_started = true;
        const esp_timer_create_args_t args = {
            .callback = alert_poll_cb,
            .name = "stk_alert",
        };
        esp_timer_handle_t th = NULL;
        if (esp_timer_create(&args, &th) == ESP_OK) {
            esp_timer_start_periodic(th, 10 * 1000 * 1000);
        }
    }
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

            lv_label_set_text_fmt(s_lbl_clock, "%02d:%02d:%02d",
                                  t.tm_hour, t.tm_min, t.tm_sec);
            static const char *wd[] = { "周日", "周一", "周二", "周三",
                                        "周四", "周五", "周六" };
            lv_label_set_text_fmt(s_lbl_date, "%02d-%02d %s",
                                  t.tm_mon + 1, t.tm_mday, wd[t.tm_wday]);
        }
    }

    if (s_dirty) {
        s_dirty = false;
        apply_data();
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

    /* 每 3 秒刷新行情 */
    static int cnt;
    if (++cnt >= 6) {
        cnt = 0;
        if (wifi_mgr_get_state() == WIFI_MGR_CONNECTED) {
            fetch_now();
        }
    }
}

static void on_show(void)
{
    s_visible = true;
    ui_shell_set_tick_cb(tick);
    fetch_now();
}

static void on_hide(void)
{
    s_visible = false;
    ui_shell_set_tick_cb(NULL);
}


const ui_app_t ui_app_stock = {
    .name = "交易",
    .symbol = LV_SYMBOL_BARS,
    .color = 0x8A3AB2,
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};
