/*
 * 设备运行日志实现 —— 见 runlog.h 的字段说明
 *
 * 线程模型
 *   runlog_event() 只做格式化 + 入队（不碰文件），可以在任何任务/回调里调；
 *   真正的 fputs 在 rl_task（优先级 1）里做，避免卡住摄像头/UI 线程。
 *
 * 掉电安全
 *   每 2 秒 fflush 一次，最坏丢 2 秒数据；文件句柄常开，减少 open/close 开销。
 */

#include "runlog.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "app_storage.h"

#define TAG "runlog"

#define Q_LEN        32
#define MSG_MAX      320
#define PATH_MAX_LEN 96
#define DIR_MAX_LEN  48

#define FILE_MAX_BYTES   (4 * 1024 * 1024)   /* 单文件 4MB 滚动 */
#define KEEP_DAYS        14                  /* 保留最近 14 天 */
#define FLUSH_PERIOD_US  (2 * 1000 * 1000)   /* 2 秒刷一次 */
#define STALL_MS         250                 /* 日志任务被延迟超过这个值算卡顿 */
#define HEAP_LOW_KB      20                  /* 内部堆低于此值告警 */

typedef struct {
    char s[MSG_MAX];
} rl_msg_t;

static QueueHandle_t s_q;
static bool s_inited;
static bool s_ready;

static char s_dir[DIR_MAX_LEN];
static char s_path[PATH_MAX_LEN];
static FILE *s_fp;
static size_t s_bytes;
static int  s_day_key = -1;      /* YYYYMMDD，用于跨天切文件 */
static int  s_seq;               /* 当天第几个文件（>0 时文件名带 -N） */
static uint32_t s_dropped;
static bool s_space_low;          /* 卡满：暂停写，等腾出空间再恢复 */

/* 心跳对比用的上次状态 */
static char s_last_wifi[32];
static char s_last_ip[24];
static bool s_heap_low_reported;

/* ================= 小工具 ================= */

static void ts_now(char *out, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year + 1900 < 2025) {
        strlcpy(out, "-", n);           /* RTC 还没同步，时间不可信 */
        return;
    }
    /* 缓冲 64 足够："YYYY-MM-DD HH:MM:SS.mmm" 最长 23 字符；
     * 用 %04d 等限定宽度，避免 -Wformat-truncation 认为 int 可能撑爆缓冲 */
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
             (tm.tm_year + 1900) % 10000, (tm.tm_mon + 1) % 100, tm.tm_mday % 100,
             tm.tm_hour % 100, tm.tm_min % 100, tm.tm_sec % 100,
             (int)((esp_timer_get_time() / 1000) % 1000));
}

/* RTC 在联网对时前是 1970，用它命名会把所有日志塞进一个文件 */
static bool time_synced(void)
{
    return time(NULL) > 1735689600;      /* 2025-01-01 */
}

static int day_key_now(time_t when)
{
    struct tm tm;
    localtime_r(&when, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

/* ================= 文件管理 ================= */

static void close_file(void)
{
    if (s_fp) {
        fflush(s_fp);
        fclose(s_fp);
        s_fp = NULL;
    }
}

static bool open_current(void)
{
    close_file();
    if (!time_synced()) {
        /* 还没对时：先写 run-unsync.jsonl，等联网拿到时间再切到当天文件 */
        snprintf(s_path, sizeof(s_path), "%s/run-unsync.jsonl", s_dir);
    } else if (s_seq > 0) {
        snprintf(s_path, sizeof(s_path), "%s/run-%08d-%d.jsonl", s_dir, s_day_key, s_seq);
    } else {
        snprintf(s_path, sizeof(s_path), "%s/run-%08d.jsonl", s_dir, s_day_key);
    }
    s_fp = fopen(s_path, "a");
    if (!s_fp) {
        ESP_LOGW(TAG, "open %s failed", s_path);
        return false;
    }
    /* 续写已有文件时要接着算大小，避免无限增长 */
    fseek(s_fp, 0, SEEK_END);
    long sz = ftell(s_fp);
    s_bytes = (sz > 0) ? (size_t)sz : 0;
    return true;
}

/* 删除超过 KEEP_DAYS 天的旧文件；文件名格式 run-YYYYMMDD*.jsonl */
static void cleanup_old(void)
{
    time_t cutoff_t = time(NULL) - (time_t)KEEP_DAYS * 86400;
    int cutoff = day_key_now(cutoff_t);
    DIR *d = opendir(s_dir);
    if (!d) {
        return;
    }
    struct dirent *e;
    int removed = 0;
    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, "run-", 4) != 0 || strlen(e->d_name) < 12) {
            continue;
        }
        int day = atoi(e->d_name + 4);
        if (day > 0 && day < cutoff) {
            char full[PATH_MAX_LEN + 64];
            snprintf(full, sizeof(full), "%s/%s", s_dir, e->d_name);
            if (unlink(full) == 0) {
                removed++;
            }
        }
    }
    closedir(d);
    if (removed) {
        ESP_LOGI(TAG, "cleaned %d old log file(s)", removed);
    }
}

/* 剩余空间不足时，从最旧的文件开始删（日志优先级低于拍照存储） */
static bool ensure_free_space(void)
{
    int free_kb = 0;
    app_storage_usage(NULL, &free_kb);
    if (free_kb >= 1024) {
        return true;
    }
    /* 收集 run-*.jsonl，按文件名（含日期）升序，从旧到新删 */
    char names[32][48];
    int cnt = 0;
    DIR *d = opendir(s_dir);
    if (!d) {
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && cnt < 32) {
        if (strncmp(e->d_name, "run-", 4) == 0) {
            strlcpy(names[cnt], e->d_name, sizeof(names[cnt]));
            cnt++;
        }
    }
    closedir(d);
    for (int i = 0; i < cnt; i++) {                 /* 冒泡升序：老的在前 */
        for (int j = i + 1; j < cnt; j++) {
            if (strcmp(names[j], names[i]) < 0) {
                char tmp[48];
                strlcpy(tmp, names[i], sizeof(tmp));
                strlcpy(names[i], names[j], sizeof(names[i]));
                strlcpy(names[j], tmp, sizeof(names[j]));
            }
        }
    }
    int removed = 0;
    for (int i = 0; i < cnt && free_kb < 2048; i++) {
        /* 别把正在写的文件删了 */
        const char *base = strrchr(s_path, '/');
        if (base && strcmp(base + 1, names[i]) == 0) {
            continue;
        }
        char full[PATH_MAX_LEN + 64];
        /* %.40s：文件名最长 47，限定宽度让编译器能证明不会截断（-Wformat-truncation） */
        snprintf(full, sizeof(full), "%s/%.40s", s_dir, names[i]);
        if (unlink(full) == 0) {
            removed++;
            app_storage_usage(NULL, &free_kb);
        }
    }
    if (removed) {
        ESP_LOGW(TAG, "free space low: removed %d old log(s), now %d KB", removed, free_kb);
    }
    return free_kb >= 256;      /* 连 256KB 都没有就别写了，免得挤掉拍照 */
}

/* 跨天 / 超容量时换文件 */
static void rotate_if_needed(void)
{
    if (s_day_key == 0) {
        /* 首次拿到真实时间：把 unsync 文件留着，新数据写进当天文件 */
        if (time_synced()) {
            s_day_key = day_key_now(time(NULL));
            s_seq = 0;
            open_current();
        }
        return;
    }
    int today = day_key_now(time(NULL));
    if (today != s_day_key) {
        s_day_key = today;
        s_seq = 0;
        open_current();
        return;
    }
    if (s_bytes >= FILE_MAX_BYTES) {
        s_seq++;
        open_current();
    }
}

/* ================= 写路径 ================= */

/* 只允许任务内部调用 */
static void write_raw(const char *line)
{
    if (!s_fp) {
        return;                     /* 已暂停（卡满）或未打开，静默丢弃 */
    }
    size_t n = strlen(line);
    if (s_bytes + n + 1 > FILE_MAX_BYTES) {
        s_seq++;
        if (!open_current()) {
            return;
        }
    }
    fputs(line, s_fp);
    fputc('\n', s_fp);
    s_bytes += n + 1;
}

/* 拼一条完整记录：{"t":..,"up":..,"lvl":..,"mod":..,"ev":..,<extra>} */
static int build_line(char *out, size_t n, runlog_level_t lvl,
                      const char *mod, const char *ev, const char *extra)
{
    char ts[64];
    ts_now(ts, sizeof(ts));
    const char *lv = (lvl == RUNLOG_E) ? "E" : (lvl == RUNLOG_W) ? "W" : "I";
    return snprintf(out, n,
                    "{\"t\":\"%s\",\"up\":%lld,\"lvl\":\"%s\",\"mod\":\"%s\",\"ev\":\"%s\"%s%s}",
                    ts, (long long)(esp_timer_get_time() / 1000000), lv, mod, ev,
                    (extra && extra[0]) ? "," : "",
                    (extra && extra[0]) ? extra : "");
}

static void rl_task(void *arg)
{
    (void)arg;
    rl_msg_t m;
    int64_t last_tick = esp_timer_get_time();
    int64_t last_flush = last_tick;
    int64_t last_space = last_tick;

    while (1) {
        bool got = (xQueueReceive(s_q, &m, pdMS_TO_TICKS(100)) == pdTRUE);
        int64_t now = esp_timer_get_time();

        if (got) {
            rotate_if_needed();
            write_raw(m.s);
        } else {
            /* 队列空转说明这 100ms 是"正常等待"；如果实际过去远多于 100ms，
             * 说明整个系统被长时间抢占/阻塞（摄像头重负载、SD 卡慢、死锁前兆）。 */
            int64_t dt_ms = (now - last_tick) / 1000;
            if (dt_ms > STALL_MS) {
                char line[MSG_MAX];
                char extra[96];
                snprintf(extra, sizeof(extra), "\"delay_ms\":%d,\"heap_int_kb\":%u",
                         (int)dt_ms,
                         (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
                build_line(line, sizeof(line), RUNLOG_W, "sys", "stall", extra);
                rotate_if_needed();
                write_raw(line);
            }
        }
        last_tick = now;

        /* 每 60 秒看一次剩余空间：卡快满时先删旧日志，还不行就暂停写，
         * 不能让日志把拍照/录像的位置挤掉 */
        if (now - last_space > 60LL * 1000000) {
            bool ok = ensure_free_space();
            if (!ok && !s_space_low) {
                s_space_low = true;
                /* 先把这句话落盘再关文件，否则"为什么停了"就查不出来 */
                char line[MSG_MAX];
                build_line(line, sizeof(line), RUNLOG_E, "sys", "log_paused",
                           "\"reason\":\"storage_full\"");
                write_raw(line);
                close_file();
                ESP_LOGE(TAG, "storage almost full, logging paused");
            } else if (ok && s_space_low) {
                s_space_low = false;
                if (open_current()) {
                    ESP_LOGI(TAG, "space recovered, logging resumed");
                }
            }
            last_space = now;
        }

        if (now - last_flush > FLUSH_PERIOD_US) {
            if (s_fp) {
                fflush(s_fp);
            }
            last_flush = now;
        }
    }
}

/* ================= 对外接口 ================= */

bool runlog_ready(void)
{
    return s_ready;
}

const char *runlog_path(void)
{
    return s_path;
}

bool runlog_init(void)
{
    if (s_inited) {
        return s_ready;
    }
    s_inited = true;

    if (!app_storage_ready()) {
        ESP_LOGW(TAG, "storage not ready, runlog disabled");
        return false;
    }
    const char *base = app_storage_is_sd() ? "/sdcard" : "/storage";
    snprintf(s_dir, sizeof(s_dir), "%s/LOGS", base);
    mkdir(s_dir, 0775);          /* 已存在返回 EEXIST，忽略 */

    s_day_key = time_synced() ? day_key_now(time(NULL)) : 0;
    s_seq = 0;
    cleanup_old();
    ensure_free_space();          /* 卡快满时先清旧的，再决定能不能写 */
    if (!open_current()) {
        return false;
    }

    s_q = xQueueCreate(Q_LEN, sizeof(rl_msg_t));
    if (!s_q) {
        ESP_LOGE(TAG, "queue create failed");
        close_file();
        return false;
    }
    BaseType_t ok = xTaskCreate(rl_task, "runlog", 4096, NULL, 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vQueueDelete(s_q);
        s_q = NULL;
        close_file();
        return false;
    }
    s_ready = true;
    ESP_LOGI(TAG, "runlog ready: %s (%s, %d KB free)",
             s_path, app_storage_is_sd() ? "TF" : "flash",
             (int)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    return true;
}

void runlog_event(runlog_level_t lvl, const char *mod, const char *ev,
                  const char *fmt, ...)
{
    if (!s_ready || !s_q) {
        return;
    }
    rl_msg_t m;
    char extra[192] = { 0 };
    if (fmt && fmt[0]) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(extra, sizeof(extra), fmt, ap);
        va_end(ap);
    }
    int n = build_line(m.s, sizeof(m.s), lvl, mod, ev, extra);
    if (n <= 0) {
        return;
    }
    /* 日志本身不能反噬系统：队列满就丢，并计数（不阻塞调用方） */
    if (xQueueSend(s_q, &m, 0) != pdTRUE) {
        s_dropped++;
        ESP_LOGW(TAG, "queue full, dropped=%lu", (unsigned long)s_dropped);
    }
}

int runlog_dump(int lines)
{
    if (!s_ready || lines <= 0 || !s_path[0]) {
        return 0;
    }
    FILE *f = fopen(s_path, "r");
    if (!f) {
        return 0;
    }
    static char buf[8193];
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long start = (sz > 8192) ? sz - 8192 : 0;
    fseek(f, start, SEEK_SET);
    size_t n = fread(buf, 1, 8192, f);
    fclose(f);
    buf[n] = '\0';
    if (start > 0) {
        /* 起点可能落在半行中间，丢掉第一段残行 */
        char *nl = strchr(buf, '\n');
        if (nl) {
            memmove(buf, nl + 1, strlen(nl + 1) + 1);
        }
    }
    /* 从尾巴往前数 lines 个换行 */
    int seen = 0;
    char *p = buf + strlen(buf);
    while (p > buf && seen <= lines) {
        p--;
        if (*p == '\n') {
            seen++;
        }
    }
    if (*p == '\n') {
        p++;
    }
    ESP_LOGI(TAG, "---- %s (tail %d) ----", s_path, lines);
    /* 逐行打印，避免一行超长被日志系统截断 */
    char *line = p;
    int printed = 0;
    while (line && *line && printed < lines) {
        char *e = strchr(line, '\n');
        if (e) {
            *e = '\0';
        }
        ESP_LOGI(TAG, "%s", line);
        printed++;
        line = e ? e + 1 : NULL;
    }
    ESP_LOGI(TAG, "---- end ----");
    return printed;
}

void runlog_boot(int rst_reason, uint32_t boots, uint32_t crashes)
{
    if (!s_ready) {
        return;
    }
    static const char *rn[] = {
        "unknown", "poweron", "ext_reset", "sw_reset",
        "panic", "int_wdt", "task_wdt", "other_wdt",
        "deepsleep", "brownout", "jtag",
    };
    const char *rname = ((int)rst_reason >= 0 && (int)rst_reason < 11)
                        ? rn[(int)rst_reason] : "?";

    runlog_event(RUNLOG_I, "boot", "start",
                 "\"ver\":\"%s\",\"rst\":%d,\"rst_name\":\"%s\","
                 "\"boots\":%u,\"crashes\":%u,\"heap_int_kb\":%u,\"psram_kb\":%u",
                 esp_app_get_description()->version, (int)rst_reason, rname,
                 (unsigned)boots, (unsigned)crashes,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    bool bad = (rst_reason == ESP_RST_PANIC || rst_reason == ESP_RST_INT_WDT ||
                rst_reason == ESP_RST_TASK_WDT || rst_reason == ESP_RST_WDT ||
                rst_reason == ESP_RST_BROWNOUT);
    if (bad) {
        runlog_event(RUNLOG_E, "boot", "crash",
                     "\"rst\":%d,\"rst_name\":\"%s\",\"note\":\"见串口 Backtrace\"",
                     (int)rst_reason, rname);
    }
}

void runlog_heartbeat(uint32_t frames, uint32_t keyframes,
                      int rtsp_clients, uint32_t rtsp_pkts,
                      const char *wifi_state, const char *ip)
{
    if (!s_ready) {
        return;
    }
    uint32_t heap_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024;
    uint32_t heap_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024;
    uint32_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024;

    runlog_event(RUNLOG_I, "sys", "hb",
                 "\"heap_int_kb\":%u,\"heap_min_kb\":%u,\"psram_kb\":%u,"
                 "\"wifi\":\"%s\",\"ip\":\"%s\",\"rtsp_clients\":%d,"
                 "\"rtsp_pkts\":%lu,\"enc_frames\":%lu,\"enc_keyframes\":%lu",
                 (unsigned)heap_int, (unsigned)heap_min, (unsigned)psram,
                 wifi_state ? wifi_state : "-", ip ? ip : "-",
                 rtsp_clients, (unsigned long)rtsp_pkts,
                 (unsigned long)frames, (unsigned long)keyframes);

    /* WiFi 状态变化单独记一条：事后能看清"什么时候断的、断了多久" */
    const char *ws = wifi_state ? wifi_state : "-";
    const char *wip = ip ? ip : "";
    if (strcmp(ws, s_last_wifi) != 0 || strcmp(wip, s_last_ip) != 0) {
        bool first = (s_last_wifi[0] == '\0');
        strlcpy(s_last_wifi, ws, sizeof(s_last_wifi));
        strlcpy(s_last_ip, wip, sizeof(s_last_ip));
        if (!first) {
            runlog_event(RUNLOG_I, "wifi", "state",
                         "\"state\":\"%s\",\"ip\":\"%s\"", ws, wip);
        }
    }

    /* 低内存：只在"掉下去"和"恢复"时各记一条，避免刷屏 */
    if (heap_int < HEAP_LOW_KB && !s_heap_low_reported) {
        s_heap_low_reported = true;
        runlog_event(RUNLOG_W, "sys", "heap_low",
                     "\"heap_int_kb\":%u,\"heap_min_kb\":%u,"
                     "\"note\":\"SDIO/JPEG/编码可能分配失败\"",
                     (unsigned)heap_int, (unsigned)heap_min);
    } else if (heap_int > HEAP_LOW_KB * 2 && s_heap_low_reported) {
        s_heap_low_reported = false;
        runlog_event(RUNLOG_I, "sys", "heap_recover",
                     "\"heap_int_kb\":%u", (unsigned)heap_int);
    }
}
