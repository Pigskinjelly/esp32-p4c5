/*
 * 蓝牙（BLE over esp_hosted）实现
 */
#include "bt_mgr.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "bt";

#if CONFIG_BT_ENABLED

#include "esp_hosted.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static bool s_on;
static bool s_host_running;

/* ---- 手机位置回传（自定 GATT 服务，仿 NUS） ---- */
#define LOC_SVC_UUID    0x6e40, 0x0001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e
#define LOC_RX_UUID     0x6e40, 0x0002, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24dcca9e
static bt_loc_cb_t s_loc_cb;
static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_rx_handle;

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

static void bt_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    bt_advertise();
}

static void bt_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool bt_mgr_available(void) { return true; }

bool bt_mgr_is_on(void) { return s_on; }

esp_err_t bt_mgr_set(bool on)
{
    if (on == s_on) {
        return ESP_OK;
    }
    if (on) {
        esp_err_t err = esp_hosted_bt_controller_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hosted bt controller init: %s", esp_err_to_name(err));
            return err;
        }
        err = esp_hosted_bt_controller_enable();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hosted bt controller enable: %s", esp_err_to_name(err));
            esp_hosted_bt_controller_deinit(false);
            return err;
        }
        err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nimble init: %s", esp_err_to_name(err));
            esp_hosted_bt_controller_disable();
            esp_hosted_bt_controller_deinit(false);
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

        ble_hs_cfg.sync_cb = bt_on_sync;
        nimble_port_freertos_init(bt_host_task);
        s_host_running = true;
        s_on = true;
        ESP_LOGI(TAG, "bluetooth on");
    } else {
        if (s_host_running) {
            nimble_port_stop();
            nimble_port_deinit();
            s_host_running = false;
        }
        esp_hosted_bt_controller_disable();
        esp_hosted_bt_controller_deinit(false);
        s_on = false;
        ESP_LOGI(TAG, "bluetooth off");
    }
    return ESP_OK;
}

#else  /* !CONFIG_BT_ENABLED */

bool bt_mgr_available(void) { return false; }
bool bt_mgr_is_on(void) { return false; }
void bt_mgr_set_loc_cb(bt_loc_cb_t cb) { (void)cb; }

esp_err_t bt_mgr_set(bool on)
{
    (void)on;
    ESP_LOGW(TAG, "BT stack not compiled (CONFIG_BT_ENABLED=n)");
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
