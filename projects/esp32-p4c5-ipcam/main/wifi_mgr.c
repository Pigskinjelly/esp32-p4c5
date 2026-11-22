/*
 * Wi-Fi 管理器实现 —— 经 ESP32-C5 协处理器（esp_wifi_remote / esp_hosted）
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "wifi_mgr.h"

#define TAG "wifi_mgr"

#define MAX_AP_CACHE        20
#define WIFI_MAX_SAVED      5       /* 像手机一样记住最近 5 个网络 */
#define SCAN_CACHE_MS       600000  /* 兜底：10 分钟内不因"缓存过期"重复扫描 */
#define BOOT_SCAN_DELAY_MS  4000    /* 开机 4s 后扫一次（此时多半已拿到 IP） */

static wifi_mgr_state_t s_state = WIFI_MGR_IDLE;
static wifi_mgr_state_t s_state_before_scan = WIFI_MGR_IDLE;
static wifi_mgr_notify_cb_t s_notify_cb;
static void *s_notify_arg;

static esp_netif_t *s_sta_netif;
static char s_ip_str[16] = "0.0.0.0";
static char s_ssid[33];
static int  s_retry;
static bool s_user_disconnect;      /* 用户主动断开：不再自动重连 */
static bool s_radio_on = true;   /* WiFi 射频开关状态 */
static bool s_auth_failed;          /* 最近一次失败是密码/认证问题 */
static int  s_auth_fail_cnt;        /* 连续认证失败次数 */
static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_scan_timer;
static int64_t s_last_scan_ms;
/* 是否真的需要扫一次：
 *   开机置 true（只扫一次）→ 连上 WiFi 后清 false（不再扫描，省电）
 *   断开时置 true（用户要重新选网络）→ 扫完清 false
 *   手动点「扫描」置 true */
static bool s_need_scan = true;
static bool s_connect_after_scan;   /* 开机"先扫描再连接"（R-39） */

static wifi_mgr_ap_info_t s_ap_cache[MAX_AP_CACHE];
static int s_ap_count;

/* 已保存的网络（[0] 为最近使用） */
typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;
static wifi_cred_t s_cred[WIFI_MAX_SAVED];
static int s_cred_cnt;

static void notify(void)
{
    if (s_notify_cb) {
        s_notify_cb(s_notify_arg);
    }
}

/* ================= 多网络凭据存储 ================= */

static void cred_store(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed when saving credentials");
        return;
    }
    nvs_set_i32(h, "cnt", s_cred_cnt);
    for (int i = 0; i < WIFI_MAX_SAVED; i++) {
        char ks[12], kp[12];
        snprintf(ks, sizeof(ks), "s%d", i);
        snprintf(kp, sizeof(kp), "p%d", i);
        if (i < s_cred_cnt) {
            nvs_set_str(h, ks, s_cred[i].ssid);
            nvs_set_str(h, kp, s_cred[i].pass);
        } else {
            nvs_erase_key(h, ks);
            nvs_erase_key(h, kp);
        }
    }
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved %d network(s) to NVS (commit=%d)", s_cred_cnt, err);
    for (int i = 0; i < s_cred_cnt; i++) {
        ESP_LOGI(TAG, "  [%d] \"%s\" pass_len=%d", i, s_cred[i].ssid,
                 (int)strlen(s_cred[i].pass));
    }
}

static void cred_load(void)
{
    s_cred_cnt = 0;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        int32_t cnt = 0;
        if (nvs_get_i32(h, "cnt", &cnt) == ESP_OK && cnt > 0 && cnt <= WIFI_MAX_SAVED) {
            for (int i = 0; i < cnt; i++) {
                char ks[12], kp[12];
                snprintf(ks, sizeof(ks), "s%d", i);
                snprintf(kp, sizeof(kp), "p%d", i);
                size_t len = sizeof(s_cred[s_cred_cnt].ssid);
                if (nvs_get_str(h, ks, s_cred[s_cred_cnt].ssid, &len) != ESP_OK) {
                    continue;
                }
                len = sizeof(s_cred[s_cred_cnt].pass);
                if (nvs_get_str(h, kp, s_cred[s_cred_cnt].pass, &len) != ESP_OK) {
                    s_cred[s_cred_cnt].pass[0] = '\0';
                }
                s_cred_cnt++;
            }
        }
        /* 兼容旧版单条记录 */
        if (s_cred_cnt == 0) {
            char ssid[33] = { 0 }, pass[65] = { 0 };
            size_t len = sizeof(ssid);
            if (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK && ssid[0]) {
                len = sizeof(pass);
                nvs_get_str(h, "pass", pass, &len);
                strlcpy(s_cred[0].ssid, ssid, sizeof(s_cred[0].ssid));
                strlcpy(s_cred[0].pass, pass, sizeof(s_cred[0].pass));
                s_cred_cnt = 1;
                ESP_LOGI(TAG, "migrated legacy credential \"%s\"", ssid);
            }
        }
        nvs_close(h);
    }

    /* 全新设备：用编译期默认值播一条 */
    if (s_cred_cnt == 0 && strlen(CONFIG_IPCAM_WIFI_SSID) > 0) {
        strlcpy(s_cred[0].ssid, CONFIG_IPCAM_WIFI_SSID, sizeof(s_cred[0].ssid));
        strlcpy(s_cred[0].pass, CONFIG_IPCAM_WIFI_PASSWORD, sizeof(s_cred[0].pass));
        s_cred_cnt = 1;
        cred_store();
    }

    ESP_LOGI(TAG, "loaded %d saved network(s)", s_cred_cnt);
    for (int i = 0; i < s_cred_cnt; i++) {
        ESP_LOGI(TAG, "  [%d] \"%s\" pass_len=%d", i, s_cred[i].ssid,
                 (int)strlen(s_cred[i].pass));
    }
}

static int cred_find(const char *ssid)
{
    for (int i = 0; i < s_cred_cnt; i++) {
        if (strcmp(s_cred[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

int wifi_mgr_saved_count(void)
{
    return s_cred_cnt;
}

const char *wifi_mgr_saved_ssid_at(int i)
{
    if (i < 0 || i >= s_cred_cnt) {
        return "";
    }
    return s_cred[i].ssid;
}

/* 保存/更新密码，并把该网络提到最前（最近使用） */
static void cred_put(const char *ssid, const char *pass)
{
    int idx = cred_find(ssid);
    wifi_cred_t tmp;
    strlcpy(tmp.ssid, ssid, sizeof(tmp.ssid));
    strlcpy(tmp.pass, pass ? pass : "", sizeof(tmp.pass));

    if (idx < 0) {
        /* 新增：满了就丢最旧的（末尾） */
        if (s_cred_cnt < WIFI_MAX_SAVED) {
            s_cred_cnt++;
        }
        idx = s_cred_cnt - 1;
    }
    /* 先移到最前，再写入 */
    for (int i = idx; i > 0; i--) {
        s_cred[i] = s_cred[i - 1];
    }
    s_cred[0] = tmp;
    cred_store();
}

static void cred_del(const char *ssid)
{
    int idx = cred_find(ssid);
    if (idx < 0) {
        return;
    }
    for (int i = idx; i < s_cred_cnt - 1; i++) {
        s_cred[i] = s_cred[i + 1];
    }
    s_cred_cnt--;
    memset(&s_cred[s_cred_cnt], 0, sizeof(wifi_cred_t));
    cred_store();
}

const char *wifi_mgr_saved_pass(const char *ssid)
{
    int idx = cred_find(ssid);
    return (idx < 0) ? NULL : s_cred[idx].pass;
}

bool wifi_mgr_is_saved(const char *ssid)
{
    return cred_find(ssid) >= 0;
}

static void apply_wifi_config(const char *ssid, const char *pass)
{
    wifi_config_t wifi_cfg = { 0 };
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode =
        (strlen(pass) == 0) ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
}

/* 时间同步成功时打一行日志（锁屏 --:-- 排查用） */
static void sntp_sync_cb(struct timeval *tv)
{
    struct tm t;
    localtime_r(&tv->tv_sec, &t);
    ESP_LOGI(TAG, "sntp synced: %04d-%02d-%02d %02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
}

static void sntp_start(void)
{
    /* 已经启动则跳过 */
    if (esp_sntp_enabled()) {
        return;
    }
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.pool.ntp.org");
    esp_sntp_setservername(2, "ntp.tencent.com");
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "sntp started");
}

static void retry_timer_cb(void *arg)
{
    (void)arg;
    if (!s_user_disconnect && s_ssid[0] != '\0' && s_radio_on) {
        /* 真正开始连了才显示"连接中"，退避等待期间状态栏显示"未连接" */
        s_state = WIFI_MGR_CONNECTING;
        notify();
        esp_wifi_connect();
    }
}

/* 开机那一次扫描 / 掉线后那一次扫描：延时执行，避免和连接流程抢射频 */
static void scan_timer_cb(void *arg)
{
    (void)arg;
    if (!s_radio_on) {
        return;
    }
    /* 限流：一分钟内最多扫一次。否则反复掉线重连会变成不停扫描（就是之前费电的原因） */
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (s_last_scan_ms > 0 && (now_ms - s_last_scan_ms) < 60000) {
        return;
    }
    /* 这两次扫描是"确实要扫"，强制放行（开机时即使已经连上也要拿到 AP 列表） */
    s_need_scan = true;
    wifi_mgr_scan_async();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)data;
        int reason = ev ? ev->reason : -1;
        if (s_state == WIFI_MGR_CONNECTED) {
            ESP_LOGW(TAG, "connection lost (reason %d)", reason);
        }
        /* 断开了：状态先回到"未连接"，等真正开始重连再改成"连接中" */
        s_state = WIFI_MGR_IDLE;
        s_need_scan = true;                 /* 掉线才需要重新扫，方便用户换个网络 */
        snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");

        /* 密码/认证失败：连续 2 次才提示（首次连接偶发 202 是 C5 未就绪，重试即可） */
        if (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_HANDSHAKE_TIMEOUT ||
            reason == WIFI_REASON_AUTH_FAIL ||
            reason == WIFI_REASON_ASSOC_FAIL) {
            s_auth_fail_cnt++;
            if (s_auth_fail_cnt >= 2) {
                s_auth_failed = true;
                ESP_LOGE(TAG, "auth failed x%d (reason %d)，密码可能不正确", s_auth_fail_cnt, reason);
            } else {
                ESP_LOGW(TAG, "auth fail #%d (reason %d)，将重试", s_auth_fail_cnt, reason);
            }
        }

        if (s_user_disconnect) {
            s_state = WIFI_MGR_IDLE;
            notify();
            return;
        }
        /* 像手机一样持续自动重连，带指数退避（0.5s→1s→…→15s 上限），
         * 用 esp_timer 异步重连，避免阻塞 wifi 事件任务 */
        if (s_ssid[0] != '\0') {
            s_retry++;
            int shift = (s_retry - 1 > 5) ? 5 : (s_retry - 1);
            int delay_ms = 500 << shift;
            if (delay_ms > 15000) {
                delay_ms = 15000;
            }
            ESP_LOGW(TAG, "auto-reconnect \"%s\" #%d in %dms (reason %d)",
                     s_ssid, s_retry, delay_ms, reason);
            if (s_retry_timer) {
                esp_timer_stop(s_retry_timer);
                esp_timer_start_once(s_retry_timer, (uint64_t)delay_ms * 1000);
            } else {
                esp_wifi_connect();
            }
            /* 连不回去就顺手扫一次（5s 内重连成功的话 GOT_IP 会取消它） */
            if (s_scan_timer) {
                esp_timer_stop(s_scan_timer);
                esp_timer_start_once(s_scan_timer, 5000ULL * 1000);
            }
        } else {
            s_state = WIFI_MGR_IDLE;
        }
        notify();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = MAX_AP_CACHE;
        wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
        s_ap_count = 0;
        if (recs && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            /* 按 RSSI 降序排序 */
            for (int i = 0; i < (int)n; i++) {
                for (int j = i + 1; j < (int)n; j++) {
                    if (recs[j].rssi > recs[i].rssi) {
                        wifi_ap_record_t t = recs[i];
                        recs[i] = recs[j];
                        recs[j] = t;
                    }
                }
            }
            for (int i = 0; i < (int)n && s_ap_count < MAX_AP_CACHE; i++) {
                if (recs[i].ssid[0] == '\0') {
                    continue;   /* 隐藏 SSID 不显示 */
                }
                /* 同名 SSID 去重：同一个路由器常同时广播 2.4G/5G 或 mesh 多节点，
                 * 只保留信号最强的那一条（列表已按 RSSI 降序，首次出现即最强） */
                bool dup = false;
                for (int k = 0; k < s_ap_count; k++) {
                    if (strcmp(s_ap_cache[k].ssid, (const char *)recs[i].ssid) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (dup) {
                    continue;
                }
                wifi_mgr_ap_info_t *info = &s_ap_cache[s_ap_count++];
                strlcpy(info->ssid, (const char *)recs[i].ssid, sizeof(info->ssid));
                info->rssi = recs[i].rssi;
                info->authmode = recs[i].authmode;
                info->channel = recs[i].primary;
            }
            ESP_LOGI(TAG, "scan done: %d unique AP(s)", s_ap_count);

            /* 目标网络不在扫描结果里时，把扫到的都打出来：
             * 现场一看就知道是"路由器没开/改名/换频段"还是"固件扫不到"。 */
            bool found = (s_ssid[0] == '\0');
            for (int i = 0; i < s_ap_count; i++) {
                if (s_ssid[0] && strcmp(s_ap_cache[i].ssid, s_ssid) == 0) {
                    found = true;
                }
            }
            if (!found) {
                ESP_LOGW(TAG, "\"%s\" NOT in scan result, visible APs:", s_ssid);
                for (int i = 0; i < s_ap_count && i < 10; i++) {
                    ESP_LOGW(TAG, "  [%d] \"%s\" ch=%d rssi=%d auth=%d",
                             i, s_ap_cache[i].ssid, s_ap_cache[i].channel,
                             s_ap_cache[i].rssi, (int)s_ap_cache[i].authmode);
                }
            }
        }
        free(recs);
        s_last_scan_ms = esp_timer_get_time() / 1000;
        s_need_scan = false;            /* 这一轮扫完了，除非再断线/手动点扫描 */

        /* ---- R-39：开机"扫描后连接"，在这里给出确切结论 ---- */
        if (s_connect_after_scan) {
            s_connect_after_scan = false;
            const wifi_mgr_ap_info_t *tgt = NULL;
            for (int i = 0; i < s_ap_count; i++) {
                if (strcmp(s_ap_cache[i].ssid, s_ssid) == 0) {
                    tgt = &s_ap_cache[i];
                    break;
                }
            }
            if (!tgt) {
                ESP_LOGE(TAG, "*** 目标 \"%s\" 不在扫描结果里（见上方可见 AP 清单）***", s_ssid);
                ESP_LOGE(TAG, "*** 常见原因：路由器没开 / SSID 改过名 / 只开了 5G 且距离远 ***");
                ESP_LOGE(TAG, "*** 已暂停自动重连，请到 设置→WiFi 里重新选择网络 ***");
                s_state = WIFI_MGR_IDLE;
                notify();
                return;
            }
            if (tgt->authmode != WIFI_AUTH_OPEN && s_cred[0].pass[0] == '\0') {
                ESP_LOGE(TAG, "*** \"%s\" 是加密网络（auth=%d），但 NVS 里保存的密码是空的 ***",
                         s_ssid, (int)tgt->authmode);
                ESP_LOGE(TAG, "*** 这就是连不上的原因，请到 设置→WiFi 里重新输入密码 ***");
                s_state = WIFI_MGR_IDLE;
                notify();
                return;
            }
            ESP_LOGI(TAG, "target \"%s\" ok: ch=%d rssi=%d auth=%d -> connect",
                     tgt->ssid, tgt->channel, tgt->rssi, (int)tgt->authmode);
            s_state = WIFI_MGR_CONNECTING;
            esp_wifi_connect();
            notify();
            return;
        }

        /* 关键修复：不要把状态强制改成 IDLE。
         * 已连接时扫描完毕，如果这里改成 IDLE，界面会显示"未连接"（假断连），
         * 股票/天气等依赖 CONNECTED 的功能也会停摆。 */
        s_state = s_state_before_scan;
        notify();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        s_auth_failed = false;
        s_auth_fail_cnt = 0;
        s_need_scan = false;            /* 连上了就不再扫描（省电） */
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
        }
        s_state = WIFI_MGR_CONNECTED;
        ESP_LOGI(TAG, "got ip: %s  (RTSP: rtsp://%s:554/stream)", s_ip_str, s_ip_str);
        /* 有些路由器下发的 DNS 会超时（日志里 getaddrinfo 202），固定用公共 DNS，
         * 否则搜歌/天气会时好时坏 */
        if (s_sta_netif) {
            esp_netif_dns_info_t dns = { 0 };
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(223, 5, 5, 5);      /* AliDNS */
            esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
            dns.ip.u_addr.ip4.addr = ESP_IP4TOADDR(119, 29, 29, 29);   /* DNSPod */
            esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
            ESP_LOGI(TAG, "dns set to 223.5.5.5 / 119.29.29.29");
        }
        sntp_start();
        notify();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        snprintf(s_ip_str, sizeof(s_ip_str), "0.0.0.0");
    }
}

esp_err_t wifi_mgr_init(void)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "create default wifi sta netif failed");
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&init_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: 0x%x（C5 侧是否已烧录 ESP-Hosted 固件？）", ret);
        return ret;
    }

    /* 国家码 CN：默认是 "01"(world safe)，只允许 1~11 信道；
     * 路由器在 12/13 信道时会出现"扫描看不到 / 重连 NO_AP_FOUND(reason 201/210)"。 */
    {
        esp_err_t cerr = esp_wifi_set_country_code("CN", true);
        ESP_LOGI(TAG, "country code CN: %s", esp_err_to_name(cerr));
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                        wifi_event_handler, NULL, NULL));

    /* 异步重连定时器（指数退避用，不阻塞 wifi 事件任务） */
    const esp_timer_create_args_t targs = {
        .callback = retry_timer_cb,
        .name = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_retry_timer));

    /* 开机那一次扫描（延时，别和首次连接抢） */
    const esp_timer_create_args_t sargs = {
        .callback = scan_timer_cb,
        .name = "wifi_scan",
    };
    ESP_ERROR_CHECK(esp_timer_create(&sargs, &s_scan_timer));

    char ssid[33], pass[65];
    cred_load();
    /* 连接最近使用过的网络（s_cred[0]） */
    if (s_cred_cnt > 0) {
        strlcpy(ssid, s_cred[0].ssid, sizeof(ssid));
        strlcpy(pass, s_cred[0].pass, sizeof(pass));
    } else {
        ssid[0] = '\0';
        pass[0] = '\0';
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    apply_wifi_config(ssid, pass);
    /* 监控场景不希望省电导致丢包/高延迟 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: 0x%x", ret);
        return ret;
    }

    /* C5 是双频芯片，但默认只按 2.4G 扫描；路由器是 5G-only 时会一直 NO_AP_FOUND。
     * 设成 AUTO（2.4G + 5G）两种频段都能扫/连。
     * 注意：这个接口必须在 esp_wifi_start() 之后调用，在之前会返回
     * ESP_ERR_WIFI_NOT_STARTED 而且不生效（R-38）。 */
    {
        esp_err_t berr = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
        if (berr == ESP_OK) {
            ESP_LOGI(TAG, "band mode: 2.4G + 5G");
        } else {
            ESP_LOGW(TAG, "set band mode auto failed: %s（继续按默认频段）",
                     esp_err_to_name(berr));
        }
    }

    if (ssid[0] != '\0') {
        s_state = WIFI_MGR_CONNECTING;
        s_retry = 0;
        /* R-39：先扫描，再按扫描结果决定连不连。
         * 原来直接 esp_wifi_connect()，一旦 AP 不存在/密码为空就只会无限
         * NO_AP_FOUND(reason 210)，日志里看不出到底是哪种原因；而且连接期间
         * 扫描会被 ESP_ERR_WIFI_STATE 拒掉，永远拿不到 AP 列表。 */
        s_connect_after_scan = true;
        s_need_scan = true;
        s_state_before_scan = WIFI_MGR_IDLE;
        wifi_scan_config_t boot_cfg = { 0 };
        esp_err_t serr = esp_wifi_scan_start(&boot_cfg, false);
        if (serr == ESP_OK) {
            s_state = WIFI_MGR_SCANNING;
            ESP_LOGI(TAG, "wifi started, scan first then connect \"%s\"", ssid);
        } else {
            ESP_LOGW(TAG, "boot scan failed 0x%x, connect directly", serr);
            s_connect_after_scan = false;
            esp_wifi_connect();
            ESP_LOGI(TAG, "wifi started, auto-connect \"%s\"", ssid);
        }
    } else {
        ESP_LOGI(TAG, "wifi started, no saved credentials");
        s_need_scan = true;
        esp_timer_start_once(s_scan_timer, BOOT_SCAN_DELAY_MS * 1000ULL);
    }
    return ESP_OK;
}

void wifi_mgr_set_notify_cb(wifi_mgr_notify_cb_t cb, void *arg)
{
    s_notify_cb = cb;
    s_notify_arg = arg;
}

esp_err_t wifi_mgr_scan_async(void)
{
    if (!s_radio_on) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_state == WIFI_MGR_SCANNING) {
        return ESP_OK;
    }
    /* 关键省电策略：只有"该扫"的时候才扫。
     *   开机扫一次 → 连上后 s_need_scan=false → 之后反复打开 WiFi 面板都不扫
     *   断线 → s_need_scan=true → 自动扫一次
     *   手动点「扫描」→ 强扫一次
     * 再留一个 10 分钟兜底，避免真的永远拿不到新 AP。 */
    int64_t now_ms = esp_timer_get_time() / 1000;
    bool stale = (s_ap_count > 0 && (now_ms - s_last_scan_ms) > SCAN_CACHE_MS);
    if (!s_need_scan && !stale) {
        return ESP_OK;
    }
    s_state_before_scan = (s_state == WIFI_MGR_CONNECTED) ? WIFI_MGR_CONNECTED : s_state;
    wifi_scan_config_t scan_cfg = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err == ESP_OK) {
        s_state = WIFI_MGR_SCANNING;
    } else {
        /* 正在连接时扫描会被拒绝：保持 s_need_scan，等下次面板打开再试 */
        ESP_LOGW(TAG, "scan start failed: 0x%x", err);
    }
    return err;
}

/** 强制重新扫描（用户手动点「扫描」时用） */
esp_err_t wifi_mgr_scan_force(void)
{
    s_need_scan = true;
    s_last_scan_ms = 0;
    return wifi_mgr_scan_async();
}

int wifi_mgr_get_scan_results(wifi_mgr_ap_info_t *out, int max)
{
    int n = (s_ap_count < max) ? s_ap_count : max;
    memcpy(out, s_ap_cache, n * sizeof(wifi_mgr_ap_info_t));
    return n;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    s_user_disconnect = false;
    s_auth_failed = false;
    s_auth_fail_cnt = 0;
    s_retry = 0;
    s_state = WIFI_MGR_CONNECTING;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }

    char pass[65] = { 0 };
    if (password) {
        strlcpy(pass, password, sizeof(pass));
    }
    /* R-40 防御：调用方传了空密码，但 NVS 里存着非空密码、且这个网络是加密的，
     * 就沿用已保存的密码。否则一次误操作（比如空着点「连接」）会把好密码
     * 覆盖成空串，设备从此永远 NO_AP_FOUND 且不会弹密码框。 */
    if (pass[0] == '\0') {
        const char *old = wifi_mgr_saved_pass(ssid);
        /* 只要旧密码非空就沿用，不去看扫描结果的 authmode：
         * 那个值在"扫描还没回来"时是未知的，用它判断会把加密网络误当开放。 */
        if (old && old[0] != '\0') {
            strlcpy(pass, old, sizeof(pass));
            ESP_LOGW(TAG, "empty password for \"%s\", keep saved one (len=%d)",
                     ssid, (int)strlen(pass));
        }
    }
    apply_wifi_config(ssid, pass);
    /* 保存到多网络凭据列表（同一 SSID 更新密码并置顶为最近使用） */
    cred_put(ssid, pass);
    notify();

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(TAG, "disconnect before connect: 0x%x", err);
    }
    vTaskDelay(pdMS_TO_TICKS(150));
    err = esp_wifi_connect();
    ESP_LOGI(TAG, "connecting \"%s\"", ssid);
    return err;
}

void wifi_mgr_disconnect(void)
{
    s_user_disconnect = true;
    s_state = WIFI_MGR_IDLE;
    esp_wifi_disconnect();
    notify();
}

/* ================= 射频开关 ================= */



bool wifi_mgr_is_on(void) { return s_radio_on; }

esp_err_t wifi_mgr_set_on(bool on)
{
    if (on == s_radio_on) {
        return ESP_OK;
    }
    if (on) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi start failed: 0x%x", err);
            return err;
        }
        esp_wifi_set_ps(WIFI_PS_NONE);
        s_radio_on = true;
        s_user_disconnect = false;
        if (s_cred_cnt > 0) {
            apply_wifi_config(s_cred[0].ssid, s_cred[0].pass);
            s_state = WIFI_MGR_CONNECTING;
            s_retry = 0;
            esp_wifi_connect();
        } else {
            s_state = WIFI_MGR_IDLE;
        }
        /* 重新开射频＝重来一遍：扫一次好让面板有列表 */
        s_need_scan = true;
        if (s_scan_timer) {
            esp_timer_stop(s_scan_timer);
            esp_timer_start_once(s_scan_timer, BOOT_SCAN_DELAY_MS * 1000ULL);
        }
        ESP_LOGI(TAG, "wifi radio on");
    } else {
        s_user_disconnect = true;
        s_state = WIFI_MGR_IDLE;
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
        }
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_radio_on = false;
        s_ap_count = 0;
        ESP_LOGI(TAG, "wifi radio off (power save)");
    }
    notify();
    return ESP_OK;
}

/* 忘记网络：断开并删除该网络的凭据，下次开机不再自动连接 */
/* 忘记某个已保存的网络（不动当前连接，除非就是它） */
esp_err_t wifi_mgr_forget_ssid(const char *ssid)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    bool is_current = (strcmp(s_ssid, ssid) == 0);
    int idx = cred_find(ssid);
    if (idx < 0 && !is_current) {
        return ESP_ERR_NOT_FOUND;
    }
    if (is_current) {
        s_user_disconnect = true;
        s_auth_failed = false;
        s_state = WIFI_MGR_IDLE;
        if (s_retry_timer) {
            esp_timer_stop(s_retry_timer);
        }
        esp_wifi_disconnect();
        s_ssid[0] = '\0';
    }
    if (idx >= 0) {
        cred_del(ssid);
    }
    ESP_LOGI(TAG, "forgot \"%s\", %d saved left", ssid, s_cred_cnt);
    notify();
    return ESP_OK;
}

void wifi_mgr_forget(void)
{
    s_user_disconnect = true;
    s_auth_failed = false;
    s_state = WIFI_MGR_IDLE;
    if (s_retry_timer) {
        esp_timer_stop(s_retry_timer);
    }
    esp_wifi_disconnect();
    if (s_ssid[0] != '\0') {
        cred_del(s_ssid);
    }
    s_ssid[0] = '\0';
    ESP_LOGI(TAG, "forgot network, %d saved left", s_cred_cnt);
    notify();
}

wifi_mgr_state_t wifi_mgr_get_state(void) { return s_state; }
const char *wifi_mgr_get_ssid(void)       { return s_ssid; }
const char *wifi_mgr_get_ip(void)         { return s_ip_str; }

bool wifi_mgr_auth_failed(void) { return s_auth_failed; }

int wifi_mgr_get_rssi(void)
{
    if (s_state != WIFI_MGR_CONNECTED) {
        return 0;
    }
    wifi_ap_record_t rec;
    if (esp_wifi_sta_get_ap_info(&rec) == ESP_OK) {
        return rec.rssi;
    }
    return 0;
}

const char *wifi_mgr_state_str(wifi_mgr_state_t st)
{
    switch (st) {
    case WIFI_MGR_CONNECTED:  return "已连接";
    case WIFI_MGR_CONNECTING: return "连接中";
    case WIFI_MGR_SCANNING:   return "扫描中";
    default:                  return "未连接";
    }
}
