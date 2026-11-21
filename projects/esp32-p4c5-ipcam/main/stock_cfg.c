#include <stdbool.h>
#include "esp_log.h"
#include "nvs.h"
#include "stock_cfg.h"

static const char *TAG = "stock_cfg";

static int  s_front = 1;
static int  s_bg = 3;
static bool s_loaded;

static void cfg_save(const char *key, int v)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, (int32_t)v);
        nvs_commit(h);
        nvs_close(h);
    }
}

void stock_cfg_load(void)
{
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    s_front = 1;
    s_bg = 3;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(h, "stk_front", &v) == ESP_OK && v >= 1 && v <= 60) {
            s_front = (int)v;
        }
        if (nvs_get_i32(h, "stk_bg", &v) == ESP_OK && v >= 1 && v <= 600) {
            s_bg = (int)v;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "refresh front=%ds bg=%ds", s_front, s_bg);
}

int stock_cfg_front(void)
{
    stock_cfg_load();
    return s_front;
}

int stock_cfg_bg(void)
{
    stock_cfg_load();
    return s_bg;
}

void stock_cfg_set_front(int sec)
{
    if (sec < 1) {
        sec = 1;
    }
    if (sec > 60) {
        sec = 60;
    }
    s_front = sec;
    s_loaded = true;
    cfg_save("stk_front", sec);
    ESP_LOGI(TAG, "front -> %ds", sec);
}

void stock_cfg_set_bg(int sec)
{
    if (sec < 1) {
        sec = 1;
    }
    if (sec > 600) {
        sec = 600;
    }
    s_bg = sec;
    s_loaded = true;
    cfg_save("stk_bg", sec);
    ESP_LOGI(TAG, "bg -> %ds", sec);
}
