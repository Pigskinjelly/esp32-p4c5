/*
 * 照片存储 —— 优先 TF 卡（SDMMC slot0），没插卡时回落到 flash storage 分区
 *
 * 为什么放 TF 卡：flash 只有 7MB，1080p 照片单张 300~800KB，很容易写满；
 * TF 卡容量大、也不消耗系统 flash 寿命。
 *
 * TF 卡引脚（本板）= SDMMC SLOT0：CLK=43 CMD=44 D0=39 D1=40 D2=41 D3=42
 * （SLOT1 GPIO14-19 被 C5 的 SDIO 占用，两者互不冲突）
 */
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <sys/errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "app_storage.h"

#define TAG "storage"

/* TF 卡（SDMMC SLOT0） */
#define SD_PIN_CLK      43
#define SD_PIN_CMD      44
#define SD_PIN_D0       39
#define SD_PIN_D1       40
#define SD_PIN_D2       41
#define SD_PIN_D3       42

#define SD_MOUNT_POINT  "/sdcard"
#define FLASH_MOUNT     STORAGE_MOUNT_POINT
#define PHOTO_SUBDIR    "DCIM"

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static SemaphoreHandle_t s_mount_lock;   /* R-12：开机任务与 UI 线程可能同时挂载 */
static sdmmc_card_t *s_card;
static bool s_ready;
static bool s_on_sd;
static int  s_next_id;
static char s_root[48] = FLASH_MOUNT;   /* 照片目录：/sdcard/DCIM 或 /storage */

const char *app_storage_root(void) { return s_root; }
bool app_storage_is_sd(void) { return s_on_sd; }

static void scan_next_id(void)
{
    s_next_id = 1;
    DIR *dir = opendir(s_root);
    if (!dir) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int id;
        if (sscanf(ent->d_name, "IMG_%d.JPG", &id) == 1 && id >= s_next_id) {
            s_next_id = id + 1;
        }
    }
    closedir(dir);
}

static bool mount_sd(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = 20000;   /* 先用 20MHz，稳定后可调高 */

    /* 先试 4 线，失败再试 1 线（部分 TF 卡座只接 D0） */
    for (int width = 4; width >= 1; width -= 3) {
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = width;
        slot.clk = SD_PIN_CLK;
        slot.cmd = SD_PIN_CMD;
        slot.d0 = SD_PIN_D0;
        if (width == 4) {
            slot.d1 = SD_PIN_D1;
            slot.d2 = SD_PIN_D2;
            slot.d3 = SD_PIN_D3;
        }
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot, &mcfg, &s_card);
        if (err == ESP_OK) {
            s_on_sd = true;
            snprintf(s_root, sizeof(s_root), SD_MOUNT_POINT "/" PHOTO_SUBDIR);
            mkdir(s_root, 0775);
            uint64_t total = 0, freeb = 0;
            esp_vfs_fat_info(SD_MOUNT_POINT, &total, &freeb);
            ESP_LOGI(TAG, "TF card mounted (%d-bit) %.1f GB, free %.1f GB",
                     width, total / 1073741824.0, freeb / 1073741824.0);
            return true;
        }
        ESP_LOGW(TAG, "TF mount %d-bit failed: %s", width, esp_err_to_name(err));
        s_card = NULL;
    }
    return false;
}

/* 分区是不是"从没格式化过"（全 0x00 或全 0xFF）——只有这种情况才自动格式化，
 * 有数据但挂不上就报错，绝不静默清空（R-09） */
static bool flash_part_blank(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    if (!part) {
        return false;
    }
    uint8_t sec[64];
    if (esp_partition_read(part, 0, sec, sizeof(sec)) != ESP_OK) {
        return false;
    }
    bool all_ff = true, all_00 = true;
    for (size_t i = 0; i < sizeof(sec); i++) {
        if (sec[i] != 0xFF) all_ff = false;
        if (sec[i] != 0x00) all_00 = false;
    }
    return all_ff || all_00;
}

static bool mount_flash(void)
{
    esp_vfs_fat_mount_config_t cfg = {
        .max_files = 4,
        .format_if_mount_failed = false,   /* R-09：绝不静默清空分区 */
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FLASH_MOUNT, "storage", &cfg, &s_wl);
    if (err != ESP_OK && flash_part_blank()) {
        /* 全新设备 / 分区还没建 FAT：这是唯一允许自动格式化的情况 */
        ESP_LOGW(TAG, "storage partition is blank, formatting once");
        err = esp_vfs_fat_spiflash_format_rw_wl(FLASH_MOUNT, "storage");
        if (err == ESP_OK) {
            err = esp_vfs_fat_spiflash_mount_rw_wl(FLASH_MOUNT, "storage", &cfg, &s_wl);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount %s failed: 0x%x（分区有数据但挂不上，不自动格式化）",
                 FLASH_MOUNT, err);
        return false;
    }
    s_on_sd = false;
    strlcpy(s_root, FLASH_MOUNT, sizeof(s_root));
    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(FLASH_MOUNT, &total, &freeb);
    ESP_LOGI(TAG, "flash storage mounted, free %.1f MB", freeb / 1048576.0);
    return true;
}

bool app_storage_mount(void)
{
    if (s_ready) {
        return true;
    }
    if (!s_mount_lock) {
        s_mount_lock = xSemaphoreCreateMutex();
    }
    if (s_mount_lock && xSemaphoreTake(s_mount_lock, pdMS_TO_TICKS(15000)) != pdTRUE) {
        ESP_LOGW(TAG, "mount busy, give up");
        return false;
    }
    bool ok = true;
    if (!s_ready) {
        ok = mount_sd() || mount_flash();
        if (ok) {
            s_ready = true;
            scan_next_id();
            ESP_LOGI(TAG, "photo dir %s, next id %d", s_root, s_next_id);
        } else {
            ESP_LOGE(TAG, "no storage: TF card absent and flash storage mount failed");
        }
    }
    if (s_mount_lock) {
        xSemaphoreGive(s_mount_lock);
    }
    return ok;
}

bool app_storage_ready(void) { return s_ready; }

bool app_storage_save_photo(const uint8_t *data, size_t len, char *name_out, size_t name_sz)
{
    if (!s_ready && !app_storage_mount()) {
        return false;
    }
    char name[32];
    snprintf(name, sizeof(name), "IMG_%04d.JPG", s_next_id);
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", s_root, name);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "create %s failed (errno %d)", path, errno);
        return false;
    }
    size_t written = fwrite(data, 1, len, f);
    fflush(f);
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "write short: %u/%u", (unsigned)written, (unsigned)len);
        unlink(path);
        return false;
    }
    s_next_id++;
    ESP_LOGI(TAG, "saved %s (%u KB)", path, (unsigned)(len / 1024));
    if (name_out && name_sz) {
        strlcpy(name_out, name, name_sz);
    }
    return true;
}

int app_storage_list_photos(char names[][32], int max)
{
    if (!s_ready && !app_storage_mount()) {
        return 0;
    }
    DIR *dir = opendir(s_root);
    if (!dir) {
        return 0;
    }
    int n = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && n < max) {
        if (strstr(ent->d_name, ".JPG") || strstr(ent->d_name, ".jpg")) {
            strlcpy(names[n], ent->d_name, 32);
            n++;
        }
    }
    closedir(dir);
    /* 升序（拍摄顺序） */
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(names[j], names[i]) < 0) {
                char t[32];
                strlcpy(t, names[i], 32);
                strlcpy(names[i], names[j], 32);
                strlcpy(names[j], t, 32);
            }
        }
    }
    return n;
}

int app_storage_read_photo(const char *name, uint8_t *buf, size_t buf_sz)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", s_root, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return -1;
    }
    size_t n = fread(buf, 1, buf_sz, f);
    fclose(f);
    return (int)n;
}

int app_storage_photo_size(const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", s_root, name);
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    return (int)st.st_size;
}

bool app_storage_delete_photo(const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", s_root, name);
    return unlink(path) == 0;
}

esp_err_t app_storage_format(void)
{
    if (s_on_sd || s_card) {
        ESP_LOGW(TAG, "format refused: photos are on TF card");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ready) {
        esp_vfs_fat_spiflash_unmount_rw_wl(FLASH_MOUNT, s_wl);
        s_ready = false;
        s_wl = WL_INVALID_HANDLE;
    }
    ESP_LOGW(TAG, "formatting flash storage by user request");
    esp_err_t err = esp_vfs_fat_spiflash_format_rw_wl(FLASH_MOUNT, "storage");
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
        return err;
    }
    return app_storage_mount() ? ESP_OK : ESP_FAIL;
}

void app_storage_usage(int *used_kb, int *free_kb)
{
    uint64_t total = 0, freeb = 0;
    if (s_ready) {
        esp_vfs_fat_info(s_on_sd ? SD_MOUNT_POINT : FLASH_MOUNT, &total, &freeb);
    }
    if (used_kb) {
        *used_kb = (int)((total - freeb) / 1024);
    }
    if (free_kb) {
        *free_kb = (int)(freeb / 1024);
    }
}
