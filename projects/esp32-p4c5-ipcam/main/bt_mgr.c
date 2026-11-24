/*
 * 蓝牙（BLE over esp_hosted）实现
 *
 * ── 为什么起停要放到独立任务里 ────────────────────────────────────────
 * 这套 BLE 栈跨两颗芯片：P4 上跑 NimBLE 主机栈，C5 上跑 controller，
 * 中间是 HCI over SDIO(VHCI)。一次「打开蓝牙」实际要做：
 *   同步 RPC 让 C5 初始化/使能 BT controller → NimBLE 分配内存池、建 host 任务
 *   → HCI 同步 → 注册 GATT → 开广播。
 * 耗时从几十毫秒到几秒不等，且必须串行。
 *
 * 旧实现直接在 LVGL 事件回调里同步跑完这一套，并且关闭时调用
 * nimble_port_stop()——那个函数内部是两个 BLE_NPL_TIME_FOREVER 的信号量等待。
 * 只要 HCI 没同步上或 host 任务先退出，LVGL 任务就永久挂死 → 任务看门狗重启。
 * 另外 nimble_port_stop() 返回时 host 任务还活着（刚从 nimble_port_run 返回），
 * 立刻 nimble_port_deinit() 会释放它正在引用的 eventq，属于 use-after-free。
 *
 * 现在的做法：
 *   - bt_mgr_set() 只投递请求，立即返回；UI 轮询 bt_mgr_state() 刷新开关。
 *   - 所有起停动作由 bt_worker 串行执行，卡住也只卡这个任务，UI 不受影响。
 *   - 关闭时先 ble_gap_adv_stop()，再 stop，等 host 任务真正退出后才 deinit。
 *   - 开启前检查内部 RAM，不够就明确失败，不让 BLE 把系统推到临界点。
 */
#include "bt_mgr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "bt";

#if CONFIG_BT_ENABLED

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_err.h"

#include "esp_hosted.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* ── 资源门限 ────────────────────────────────────────────────────────
 * NimBLE 的内存池(msys/ACL)是静态数组，但 host 任务栈、GATT 注册、
 * 连接对象都走内部 RAM。本机内部 RAM 同时被 LVGL 双 framebuffer、
 * 相机 DMA 描述符和 lwIP 抢，低于这个水位就不要再开了。 */
#define BT_MIN_INT_FREE_KB       32    /* 内部 RAM 总空闲 */
#define BT_MIN_INT_BLOCK_KB      16    /* 内部 RAM 最大连续块 */
#define BT_SYNC_TIMEOUT_MS       5000  /* 等 HCI 同步的上限 */
#define BT_HOST_EXIT_TIMEOUT_MS  3000  /* 等 host 任务退出的上限 */
#define BT_WORKER_STACK          6144

static volatile bt_state_t s_state = BT_STATE_OFF;
static volatile bool s_synced;              /* HCI 已同步，可以广播 */
static volatile bool s_host_alive;          /* host 任务正在 nimble_port_run 里 */
static esp_err_t     s_last_err = ESP_OK;
static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;

static QueueHandle_t     s_req_q;           /* 元素：uint8_t 0=关 1=开 */
static SemaphoreHandle_t s_sync_sem;        /* bt_on_sync 里 give */
static SemaphoreHandle_t s_host_exited;     /* host 任务从 run 返回后 give */
static TaskHandle_t      s_worker;
static bool              s_inited;

static bool s_on;                           /* 兼容旧语义：已开启 */
static bool s_host_running;
static bool s_ctrl_inited;                  /* controller 是否已成功 init */

/* ---- 手机位置回传（自定 GATT 服务，仿 NUS） ----
 * 128 位写法（大端显示）：
 *   服务 6e400001-b5a3-f393-e0a9-e50e24dcca9e
 *   RX   6e400002-b5a3-f393-e0a9-e50e24dcca9e
 * BLE_UUID128_DECLARE 要按小端逐字节填，每项必须 ≤0xFF；
 * 旧写法传 0x6e40 这类 16 位值，会被截断成 0x40 且数组只填了 6 字节，
 * 实际广播的 UUID 并不是注释里那个。这里按标准小端序列写全。 */
#define LOC_SVC_UUID    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
                        0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
#define LOC_RX_UUID     0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
                        0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e
static bt_loc_cb_t s_loc_cb;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

void bt_mgr_set_loc_cb(bt_loc_cb_t cb)
{
    s_loc_cb = cb;
}

/* 收到 "lat,lon" 文本 */
static int loc_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    char buf[48] = { 0 };
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    buf[len] = '\0';

    char *end = NULL;
    double lat = strtod(buf, &end);
    if (!end || (*end != ',' && *end != ' ' && *end != ';')) {
        ESP_LOGW(TAG, "loc parse failed: %s", buf);
        return 0;
    }
    double lon = strtod(end + 1, NULL);
    if (lat > -90.0 && lat < 90.0 && lon > -180.0 && lon < 180.0) {
        ESP_LOGI(TAG, "phone location: %.6f, %.6f", lat, lon);
        if (s_loc_cb) {
            s_loc_cb(lat, lon);
        }
    }
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(LOC_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID128_DECLARE(LOC_RX_UUID),
                .access_cb = loc_rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            { 0 },
        },
    },
    { 0 },
};

static int bt_gap_event(struct ble_gap_event *event, void *arg);

static void bt_advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)ble_svc_gap_device_name();
    fields.name_len = (uint8_t)strlen((const char *)fields.name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields rc=%d", rc);
    }

    /* 可连接广播：手机连上来才能写位置 */
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, bt_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv_start rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "BLE advertising as \"%s\" (connectable)", ble_svc_gap_device_name());
    }
}

static int bt_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_conn_handle = (event->connect.status == 0) ? event->connect.conn_handle
                                                     : BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "phone connected (status=%d)", event->connect.status);
        if (event->connect.status != 0) {
            bt_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "phone disconnected (reason=%d)", event->disconnect.reason);
        bt_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        bt_advertise();
        return 0;

    default:
        return 0;
    }
}

/* host 与 controller 失步时会被调用；没有它，同步失败是「静默」的 */
static void bt_on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "BLE host reset, reason=%d（HCI 与 C5 controller 失步）", reason);
}

static void bt_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed rc=%d", rc);
    }
    s_synced = true;
    if (s_sync_sem) {
        xSemaphoreGive(s_sync_sem);
    }
    ESP_LOGI(TAG, "BLE host synced (own_addr_type=%d)", s_own_addr_type);
    bt_advertise();
}

static void bt_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "host task started (stack=%d)", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE);
    s_host_alive = true;
    nimble_port_run();

    /* run 已返回：此时可以安全 deinit 了，先通知等待方 */
    s_host_alive = false;
    if (s_host_exited) {
        xSemaphoreGive(s_host_exited);
    }
    nimble_port_freertos_deinit();
}

bool bt_mgr_available(void) { return true; }

bt_state_t bt_mgr_state(void)
{
    bt_state_t st;
    taskENTER_CRITICAL(&s_lock);
    st = s_state;
    taskEXIT_CRITICAL(&s_lock);
    return st;
}

const char *bt_mgr_state_str(bt_state_t st)
{
    switch (st) {
    case BT_STATE_OFF:      return "off";
    case BT_STATE_STARTING: return "starting";
    case BT_STATE_ON:       return "on";
    case BT_STATE_STOPPING: return "stopping";
    case BT_STATE_ERROR:    return "error";
    default:                return "?";
    }
}

const char *bt_mgr_last_error_str(void)
{
    return s_last_err == ESP_OK ? "none" : esp_err_to_name(s_last_err);
}

bool bt_mgr_is_on(void)
{
    return bt_mgr_state() == BT_STATE_ON;
}

static void bt_set_state(bt_state_t st, esp_err_t err)
{
    taskENTER_CRITICAL(&s_lock);
    s_state = st;
    s_last_err = err;
    s_on = (st == BT_STATE_ON || st == BT_STATE_STARTING);
    taskEXIT_CRITICAL(&s_lock);
}

/* ── 真正的开启流程（只在 worker 里跑） ────────────────────────── */
static esp_err_t bt_do_start(void)
{
    size_t int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_blk  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "start: internal free=%uKB largest=%uKB",
             (unsigned)(int_free / 1024), (unsigned)(int_blk / 1024));

    /* 内存不够就别硬开：宁可明确失败，也不要事后在相机/LVGL 里随机崩 */
    if (int_free < (size_t)BT_MIN_INT_FREE_KB * 1024 ||
        int_blk  < (size_t)BT_MIN_INT_BLOCK_KB * 1024) {
        ESP_LOGE(TAG, "not enough internal RAM (free=%uKB largest=%uKB, need %d/%dKB)",
                 (unsigned)(int_free / 1024), (unsigned)(int_blk / 1024),
                 BT_MIN_INT_FREE_KB, BT_MIN_INT_BLOCK_KB);
        return ESP_ERR_NO_MEM;
    }

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_hosted_bt_controller_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hosted bt controller init: %s (hosted link up? wifi 停了吗)",
                 esp_err_to_name(err));
        return err;
    }
    s_ctrl_inited = true;
    err = esp_hosted_bt_controller_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hosted bt controller enable: %s", esp_err_to_name(err));
        return err;      /* 清理统一交给 worker 里的 bt_do_stop() */
    }
    ESP_LOGI(TAG, "controller ready in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));

    /* 清掉上一轮残留信号，避免误判同步成功 */
    if (s_sync_sem) {
        xSemaphoreTake(s_sync_sem, 0);
    }
    s_synced = false;

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init: %s", esp_err_to_name(err));
        return err;
    }

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("ESP32-P4C5");

    /* 注册位置回传服务 */
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "add gatt svcs rc=%d", rc);
    }

    ble_hs_cfg.reset_cb = bt_on_reset;
    ble_hs_cfg.sync_cb = bt_on_sync;

    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    nimble_port_freertos_init(bt_host_task);
    s_host_running = true;

    if (xSemaphoreTake(s_sync_sem, pdMS_TO_TICKS(BT_SYNC_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "BLE host did not sync in %d ms —— 多半是 C5 侧 BT 未使能/固件不匹配",
                 BT_SYNC_TIMEOUT_MS);
        s_last_err = ESP_ERR_TIMEOUT;
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "bluetooth on (%lld ms)",
             (long long)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

/* ── 真正的关闭流程（只在 worker 里跑） ────────────────────────── */
static void bt_do_stop(void)
{
    int64_t t0 = esp_timer_get_time();

    if (s_synced) {
        int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "adv_stop rc=%d", rc);
        }
        s_synced = false;
    }

    if (s_host_running) {
        /* nimble_port_stop() 里的等待是 FOREVER 的，所以在 worker 里调用。
         * 先确认 host 任务还活着，否则那两处 pend 会永远等下去。 */
        if (s_host_alive) {
            if (s_host_exited) {
                xSemaphoreTake(s_host_exited, 0);   /* 清残留 */
            }
            nimble_port_stop();
            if (xSemaphoreTake(s_host_exited, pdMS_TO_TICKS(BT_HOST_EXIT_TIMEOUT_MS)) != pdTRUE) {
                ESP_LOGE(TAG, "host task did not exit in %d ms, deinit anyway",
                         BT_HOST_EXIT_TIMEOUT_MS);
            }
        } else {
            ESP_LOGW(TAG, "host task already gone, skip nimble_port_stop");
        }
        nimble_port_deinit();
        s_host_running = false;
    }

    if (s_ctrl_inited) {
        esp_hosted_bt_controller_disable();
        esp_hosted_bt_controller_deinit(false);
        s_ctrl_inited = false;
    }
    ESP_LOGI(TAG, "bluetooth off (%lld ms)",
             (long long)((esp_timer_get_time() - t0) / 1000));
}

static void bt_worker(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "worker started");
    for (;;) {
        uint8_t req;
        if (xQueueReceive(s_req_q, &req, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (req) {
            bt_state_t cur = bt_mgr_state();
            if (cur == BT_STATE_ON) {
                bt_set_state(BT_STATE_ON, ESP_OK);
                continue;
            }
            bt_set_state(BT_STATE_STARTING, ESP_OK);
            esp_err_t err = bt_do_start();
            if (err != ESP_OK) {
                /* 起不来就把已分配的东西退回去，别留半截栈 */
                bt_do_stop();
                bt_set_state(BT_STATE_ERROR, err);
                ESP_LOGE(TAG, "bluetooth start failed: %s", esp_err_to_name(err));
            } else {
                bt_set_state(BT_STATE_ON, ESP_OK);
            }
        } else {
            bt_set_state(BT_STATE_STOPPING, ESP_OK);
            bt_do_stop();
            bt_set_state(BT_STATE_OFF, ESP_OK);
        }
    }
}

static esp_err_t bt_mgr_init_once(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool need = !s_inited;
    if (need) {
        s_inited = true;      /* 先占位，防止并发重复建 */
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!need) {
        return ESP_OK;
    }

    s_req_q = xQueueCreate(2, sizeof(uint8_t));
    s_sync_sem = xSemaphoreCreateBinary();
    s_host_exited = xSemaphoreCreateBinary();
    if (!s_req_q || !s_sync_sem || !s_host_exited) {
        ESP_LOGE(TAG, "queue/sem alloc failed");
        taskENTER_CRITICAL(&s_lock);
        s_inited = false;          /* 允许下次重试 */
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(bt_worker, "bt_worker", BT_WORKER_STACK, NULL, 4, &s_worker) != pdPASS) {
        ESP_LOGE(TAG, "worker task create failed");
        taskENTER_CRITICAL(&s_lock);
        s_inited = false;
        taskEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t bt_mgr_set(bool on)
{
    esp_err_t err = bt_mgr_init_once();
    if (err != ESP_OK) {
        return err;
    }

    bt_state_t st = bt_mgr_state();
    if (on) {
        if (st == BT_STATE_ON || st == BT_STATE_STARTING) {
            return ESP_OK;                       /* 已经是目标状态 */
        }
        if (st == BT_STATE_STOPPING) {
            return ESP_ERR_INVALID_STATE;        /* 正在关，别交叉 */
        }
    } else {
        if (st == BT_STATE_OFF || st == BT_STATE_STOPPING) {
            return ESP_OK;
        }
        if (st == BT_STATE_STARTING) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    uint8_t req = on ? 1 : 0;
    if (xQueueSend(s_req_q, &req, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    /* 立刻进入中间态，避免用户连点时重复投递 */
    bt_set_state(on ? BT_STATE_STARTING : BT_STATE_STOPPING, ESP_OK);
    return ESP_OK;
}

#else  /* !CONFIG_BT_ENABLED */

bool bt_mgr_available(void) { return false; }
bool bt_mgr_is_on(void) { return false; }
bt_state_t bt_mgr_state(void) { return BT_STATE_OFF; }
const char *bt_mgr_state_str(bt_state_t st) { (void)st; return "off"; }
const char *bt_mgr_last_error_str(void) { return "unsupported"; }
void bt_mgr_set_loc_cb(bt_loc_cb_t cb) { (void)cb; }

esp_err_t bt_mgr_set(bool on)
{
    (void)on;
    ESP_LOGW(TAG, "BT stack not compiled (CONFIG_BT_ENABLED=n)");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
