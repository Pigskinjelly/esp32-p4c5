/*
 * 音乐 App —— 网易云那种深色唱片风
 *
 * 音频链路：SD 卡 WAV（16bit PCM）→ esp_codec_dev → ES8311 → NS4150B
 * 说明：固件里没有 MP3/AAC 解码器（IDF 未提供），所以只支持 WAV。
 *       把 .wav 放到 SD 卡 /Music 目录即可（也可以用 16bit PCM 的任意 wav）。
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <errno.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"

#include "lvgl.h"
#include "ui_shell.h"
#include "ui_theme.h"

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include "esp_lvgl_port.h"
#include "esp_crt_bundle.h"
#include "gbk2utf8.h"
#include "app_audio.h"
#include "app_storage.h"
#include "nvs.h"

static const char *TAG = "ui_music";

#define MAX_TRACKS 32
#define NAME_LEN   64

static char s_tracks[MAX_TRACKS][NAME_LEN];
static char s_paths[MAX_TRACKS][96];
static int  s_count;
static int  s_index;

/* 播放控制 */
static SemaphoreHandle_t s_sem;
static volatile int  s_cmd;        /* 1 播放 2 暂停 3 停止 */
static volatile bool s_playing;
static volatile bool s_paused;     /* 网络播放暂停：保持连接、不取数据 */
static volatile int  s_pos_ms;
static volatile int  s_total_ms;
static volatile int  s_err;

/* UI */
static lv_obj_t *s_cover;
static lv_obj_t *s_lbl_title;
static lv_obj_t *s_lbl_sub;
static lv_obj_t *s_bar;
static lv_obj_t *s_lbl_cur;
static lv_obj_t *s_lbl_total;
static lv_obj_t *s_btn_play;
static lv_obj_t *s_lbl_play;
static lv_obj_t *s_playlist;
static lv_obj_t *s_plist;        /* 列表容器（必须在创建时记住，不能按序号取） */
static lv_obj_t *s_lbl_hint;
static lv_obj_t *s_lbl_empty;
static lv_obj_t *s_search;
static lv_obj_t *s_search_ta;
static lv_obj_t *s_search_list;
static char s_search_kw[32];
static lv_obj_t *s_vol_bar;
static lv_obj_t *s_btn_mute;
static int s_vol = 5;
static int s_vol_before_mute = 5;

static void vol_save(int v)
{
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "vol", v);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void vol_apply(int v)
{
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_vol = v;
    app_audio_set_volume(v);
    if (s_vol_bar) {
        lv_slider_set_value(s_vol_bar, v, LV_ANIM_OFF);
    }
    if (s_btn_mute) {
        lv_obj_set_style_bg_color(s_btn_mute, col(v == 0 ? 0xB0304A : 0x1A2230), 0);
    }
}

static void vol_slider_cb(lv_event_t *e)
{
    int v = (int)lv_slider_get_value(lv_event_get_target(e));
    vol_apply(v);
    vol_save(v);
    if (v > 0) {
        s_vol_before_mute = v;
    }
}

static void mute_cb(lv_event_t *e)
{
    (void)e;
    if (s_vol > 0) {
        s_vol_before_mute = s_vol;
        vol_apply(0);
    } else {
        vol_apply(s_vol_before_mute > 0 ? s_vol_before_mute : 5);
    }
    vol_save(s_vol);
}
static lv_obj_t *s_radio;
static volatile char s_stream_url[128];   /* 非空则播放网络电台 */
static lv_obj_t *s_lbl_fav;
static lv_obj_t *s_lbl_dl;

/* 当前正在播的曲目（本地或网络），收藏/下载都基于它 */
static char s_cur_name[NAME_LEN];
static char s_cur_url[128];

/* ---- 收藏（NVS：每行 "名字<TAB>网址"，网址为空表示本地文件） ---- */
#define FAV_MAX 24
static char s_fav_name[FAV_MAX][NAME_LEN];
static char s_fav_url[FAV_MAX][112];
static int  s_fav_cnt;

static void fav_load(void)
{
    s_fav_cnt = 0;
    char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return;
    }
    buf[0] = 0;
    nvs_handle_t h;
    size_t len = 2048;
    if (nvs_open("sys", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "music_fav2", buf, &len);
        nvs_close(h);
    }
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line && s_fav_cnt < FAV_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = 0;
            strlcpy(s_fav_url[s_fav_cnt], tab + 1, sizeof(s_fav_url[0]));
        } else {
            s_fav_url[s_fav_cnt][0] = 0;
        }
        strlcpy(s_fav_name[s_fav_cnt], line, sizeof(s_fav_name[0]));
        if (s_fav_name[s_fav_cnt][0]) {
            s_fav_cnt++;
        }
    }
    heap_caps_free(buf);
    ESP_LOGI(TAG, "favorites loaded: %d", s_fav_cnt);
}

static void fav_save(void)
{
    char *buf = heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return;
    }
    size_t o = 0;
    buf[0] = 0;
    for (int i = 0; i < s_fav_cnt && o < 2000; i++) {
        int n = snprintf(buf + o, 2048 - o, "%s%s\t%s", i ? "\n" : "",
                         s_fav_name[i], s_fav_url[i]);
        if (n <= 0) {
            break;
        }
        o += (size_t)n;
    }
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "music_fav2", buf);
        nvs_commit(h);
        nvs_close(h);
    }
    heap_caps_free(buf);
}

static bool is_fav(const char *name)
{
    if (!name || !name[0]) {
        return false;
    }
    for (int i = 0; i < s_fav_cnt; i++) {
        if (strcmp(s_fav_name[i], name) == 0) {
            return true;
        }
    }
    return false;
}

/* 返回 true 表示现在已在收藏里 */
static bool fav_toggle(const char *name, const char *url)
{
    if (!name || !name[0]) {
        return false;
    }
    for (int i = 0; i < s_fav_cnt; i++) {
        if (strcmp(s_fav_name[i], name) == 0) {
            for (int k = i; k < s_fav_cnt - 1; k++) {
                strlcpy(s_fav_name[k], s_fav_name[k + 1], sizeof(s_fav_name[0]));
                strlcpy(s_fav_url[k], s_fav_url[k + 1], sizeof(s_fav_url[0]));
            }
            s_fav_cnt--;
            fav_save();
            return false;
        }
    }
    if (s_fav_cnt >= FAV_MAX) {
        ui_shell_toast("收藏已满，先取消几个");
        return true;
    }
    strlcpy(s_fav_name[s_fav_cnt], name, sizeof(s_fav_name[0]));
    strlcpy(s_fav_url[s_fav_cnt], url ? url : "", sizeof(s_fav_url[0]));
    s_fav_cnt++;
    fav_save();
    return true;
}

/* ================= WAV 解析 ================= */
typedef struct {
    int rate;
    int channels;
    int bits;
    long data_off;
    long data_len;
} wav_info_t;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static bool wav_parse(FILE *f, wav_info_t *info)
{
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    bool have_fmt = false;
    while (1) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) {
            break;
        }
        uint32_t sz = rd32(ch + 4);
        if (memcmp(ch, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            size_t n = sz < 16 ? sz : 16;
            if (fread(fmt, 1, n, f) != n) {
                return false;
            }
            info->channels = rd16(fmt + 2);
            info->rate = (int)rd32(fmt + 4);
            info->bits = rd16(fmt + 14);
            have_fmt = true;
            if (sz > n) {
                fseek(f, (long)(sz - n), SEEK_CUR);
            }
        } else if (memcmp(ch, "data", 4) == 0) {
            info->data_off = ftell(f);
            info->data_len = (long)sz;
            if (have_fmt) {
                return true;
            }
            fseek(f, (long)sz, SEEK_CUR);
        } else {
            fseek(f, (long)sz, SEEK_CUR);
        }
        if (sz & 1) {
            fseek(f, 1, SEEK_CUR);
        }
    }
    return false;
}

/* 只保留 ASCII 可见字符 / 中文 / 全角标点，其余（emoji 等）丢弃 */
static void sanitize_name(char *s)
{
    char *r = s;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned char c = *p;
        unsigned int cp;
        int n;
        if (c < 0x80) {
            cp = c; n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F; n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F; n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07; n = 4;
        } else {
            p++; continue;
        }
        bool ok = true;      /* 之前写成 (n == 1)：多字节字符永远判为非法，中文全被丢掉 */
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!ok) { p++; continue; }
        bool keep = (cp >= 0x20 && cp < 0x7F) ||
                    (cp >= 0x4E00 && cp <= 0x9FFF) ||
                    (cp >= 0x3000 && cp <= 0x303F) ||
                    (cp >= 0xFF01 && cp <= 0xFF5E);
        if (keep) {
            memcpy(r, p, (size_t)n);
            r += n;
        } else if (cp == 0x20 || cp == 0xA0) {
            *r++ = ' ';
        }
        p += n;
    }
    *r = 0;
}

static bool ext_is(const char *n, const char *ext)
{
    size_t l = strlen(n), e2 = strlen(ext);
    if (l < e2 + 1) {
        return false;
    }
    const char *p = n + l - e2;
    for (size_t i = 0; i < e2; i++) {
        char a = p[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return p[-1] == '.';
}

static bool is_audio(const char *n)
{
    return ext_is(n, "mp3") || ext_is(n, "wav");
}


/* ================= 网络电台（HTTP MP3 流） ================= */
#include "esp_http_client.h"

static void stream_http_mp3(const char *url)
{
    const int cap = 24 * 1024;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    mp3dec_t *dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
    if (!buf || !pcm || !dec) {
        if (buf) heap_caps_free(buf);
        if (pcm) heap_caps_free(pcm);
        if (dec) heap_caps_free(dec);
        s_err = 4;
        return;
    }
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 15000, .buffer_size = 4096,
                                     .user_agent = "Mozilla/5.0",
                                     .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        heap_caps_free(buf); heap_caps_free(pcm); heap_caps_free(dec);
        s_err = 4;
        return;
    }
    ESP_LOGI(TAG, "stream: %s", url);
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        s_err = 5;
        goto out;
    }
    int clen = (int)esp_http_client_fetch_headers(cli);
    ESP_LOGI(TAG, "stream content-length=%d", clen);
    mp3dec_init(dec);
    int rate = 0, ch = 0;
    size_t len = 0;
    bool dur_ok = false;
    while (s_playing && s_cmd == 0) {
        if (s_paused) {
            vTaskDelay(pdMS_TO_TICKS(40));   /* 暂停：不读不写，连接留着 */
            continue;
        }
        if (len < (size_t)cap) {
            int r = esp_http_client_read(cli, (char *)buf + len, cap - len);
            if (r <= 0) {
                break;               /* 流断开会重连 */
            }
            len += (size_t)r;
        }
        uint8_t *p = buf;
        size_t left = len;
        bool consumed = false;
        while (left > 0 && !s_paused) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(dec, p, (int)left, pcm, &info);
            if (info.frame_bytes <= 0) {
                break;
            }
            p += info.frame_bytes;
            left -= info.frame_bytes;
            consumed = true;
            if (samples > 0) {
                if (info.hz != rate || info.channels != ch) {
                    rate = info.hz;
                    ch = info.channels;
                    app_audio_out_format(rate, ch);
                }
                app_audio_out_write(pcm, (size_t)samples * (size_t)ch * 2);
                /* 播放进度：网络流原来完全没更新，UI 一直是 0:00 */
                s_pos_ms += (int)((int64_t)samples * 1000 / (rate > 0 ? rate : 44100));
                if (!dur_ok && clen > 0 && info.bitrate_kbps > 0) {
                    /* 有 Content-Length 就能按比特率估出总时长 */
                    s_total_ms = (int)((int64_t)clen * 8 * 1000 /
                                       ((int64_t)info.bitrate_kbps * 1000));
                    dur_ok = true;
                }
            }
        }
        if (consumed) {
            memmove(buf, p, left);
            len = left;
        } else if (len == (size_t)cap) {
            len = 0;                 /* 缓冲里没同步头，丢掉 */
        }
    }
    s_err = 0;
out:
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    heap_caps_free(buf);
    heap_caps_free(pcm);
    heap_caps_free(dec);
}

/* ================= MP3 流式解码 ================= */
static void stream_mp3(FILE *f)
{
    const int cap = 24 * 1024;
    uint8_t *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    mp3dec_t *dec = heap_caps_malloc(sizeof(mp3dec_t), MALLOC_CAP_SPIRAM);
    if (!buf || !pcm || !dec) {
        ESP_LOGE(TAG, "mp3: no memory");
        if (buf) heap_caps_free(buf);
        if (pcm) heap_caps_free(pcm);
        if (dec) heap_caps_free(dec);
        s_err = 4;
        return;
    }
    mp3dec_init(dec);
    int rate = 0, ch = 0;
    long fsize = 0;
    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    size_t len = fread(buf, 1, cap, f);
    uint8_t *p = buf;
    size_t left = len;
    bool dur_ok = false;

    while (s_playing && s_cmd == 0) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, p, (int)left, pcm, &info);
        if (info.frame_bytes <= 0) {
            /* 数据不够：再读一些 */
            if (left < (size_t)cap) {
                memmove(buf, p, left);
                size_t r = fread(buf + left, 1, cap - left, f);
                if (r == 0) {
                    break;
                }
                p = buf;
                left += r;
                continue;
            }
            break;
        }
        p += info.frame_bytes;
        left -= info.frame_bytes;

        if (samples > 0) {
            if (info.hz != rate || info.channels != ch) {
                rate = info.hz;
                ch = info.channels;
                app_audio_out_format(rate, ch);
                if (!dur_ok && info.bitrate_kbps > 0) {
                    s_total_ms = (int)(fsize * 8 / (info.bitrate_kbps * 1000.0) * 1000);
                    dur_ok = true;
                }
            }
            app_audio_out_write(pcm, (size_t)samples * (size_t)ch * 2);
            s_pos_ms += (int)((int64_t)samples * 1000 / rate);
        }
        if (left < 4096) {
            memmove(buf, p, left);
            size_t r = fread(buf + left, 1, cap - left, f);
            p = buf;
            left += r;
            if (left == 0) {
                break;
            }
        }
    }
    heap_caps_free(buf);
    heap_caps_free(pcm);
    heap_caps_free(dec);
}

/* ================= 播放任务 ================= */
/* 酷我播放地址解析（实现见文件后半段的搜索部分） */
static bool kuwo_resolve(const char *rid, char *out, size_t cap);

static void player_task(void *arg)
{
    (void)arg;
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no buffer");
        vTaskDeleteWithCaps(NULL);
        return;
    }

    int last_idx = -1;
    while (1) {
        xSemaphoreTake(s_sem, portMAX_DELAY);

        int cmd = s_cmd;
        s_cmd = 0;
        if (cmd == 3 || cmd == 0) {
            s_playing = false;
            continue;
        }
        if (cmd == 2) {          /* 暂停：直接停流，恢复时重新从当前位置开始 */
            s_playing = false;
            continue;
        }

        /* 网络歌曲 / 电台：循环重连直到用户停止/切歌 */
        if (s_stream_url[0]) {
            s_paused = false;
            char resolved[256];
            const char *play_url = (const char *)s_stream_url;

            /* 酷我的地址是临时的，播放前现取一次 */
            if (strncmp(play_url, "kuwo://", 7) == 0) {
                if (!kuwo_resolve(play_url + 7, resolved, sizeof(resolved))) {
                    ESP_LOGW(TAG, "kuwo resolve failed, abort");
                    s_err = 6;
                    s_playing = false;
                    continue;
                }
                play_url = resolved;
            }

            s_total_ms = 0;
            s_pos_ms = 0;
            s_err = 0;
            s_playing = true;
            while (s_playing && s_cmd == 0) {
                stream_http_mp3(play_url);
                if (!s_playing || s_cmd != 0) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(800));
            }
            s_playing = false;
            continue;
        }

        /* 播放（从当前位置或从头） */
        if (s_index < 0 || s_index >= s_count) {
            continue;
        }
        FILE *f = fopen(s_paths[s_index], "rb");
        if (!f) {
            ESP_LOGE(TAG, "open %s failed", s_paths[s_index]);
            s_err = 1;
            s_playing = false;
            continue;
        }
        if (ext_is(s_paths[s_index], "mp3")) {
            s_total_ms = 0;
            s_pos_ms = 0;
            s_err = 0;
            s_playing = true;
            last_idx = s_index;
            ESP_LOGI(TAG, "play mp3 %s", s_tracks[s_index]);
            stream_mp3(f);
            fclose(f);
            s_playing = false;
            if (s_cmd == 0 && s_err == 0 && s_index == last_idx && s_count > 1) {
                s_index = (s_index + 1) % s_count;
                s_cmd = 1;
                xSemaphoreGive(s_sem);
            }
            continue;
        }

        wav_info_t wi;
        if (!wav_parse(f, &wi) || wi.bits != 16 || wi.channels < 1 || wi.channels > 2 || wi.rate <= 0) {
            ESP_LOGE(TAG, "%s: only 16bit PCM wav supported", s_paths[s_index]);
            s_err = 2;
            s_playing = false;
            fclose(f);
            continue;
        }
        if (app_audio_out_format(wi.rate, wi.channels) != ESP_OK) {
            s_err = 3;
            s_playing = false;
            fclose(f);
            continue;
        }
        int frame = wi.channels * 2;
        s_total_ms = (int)(wi.data_len / frame * 1000LL / wi.rate);
        s_pos_ms = 0;
        s_playing = true;
        s_err = 0;
        last_idx = s_index;
        ESP_LOGI(TAG, "play %s (%d Hz, %d ch, %d s)", s_tracks[s_index], wi.rate, wi.channels,
                 s_total_ms / 1000);

        while (s_playing && s_cmd == 0) {
            size_t n = fread(buf, 1, 4096, f);
            if (n == 0) {
                break;           /* 放完了 */
            }
            int wr = app_audio_out_write(buf, n);
            if (wr < 0) {
                break;
            }
            s_pos_ms += (int)(n / frame * 1000LL / wi.rate);
        }
        fclose(f);
        s_playing = false;

        /* 自然播放结束：自动下一首 */
        if (s_cmd == 0 && s_err == 0 && s_index == last_idx) {
            if (s_count > 1) {
                s_index = (s_index + 1) % s_count;
                s_cmd = 1;
                xSemaphoreGive(s_sem);
            }
        }
    }
}

static void play_index(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return;
    }
    s_index = idx;
    s_stream_url[0] = 0;
    s_paused = false;
    s_pos_ms = 0;
    strlcpy(s_cur_name, s_tracks[idx], sizeof(s_cur_name));
    s_cur_url[0] = 0;
    app_audio_out_flush();          /* 丢掉上一首还没播完的采样 */
    s_cmd = 1;
    if (s_sem) {
        xSemaphoreGive(s_sem);
    }
}

/* ================= 扫描歌曲 ================= */
static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && s_count < MAX_TRACKS) {
        if (ent->d_name[0] == '.' || !is_audio(ent->d_name)) {
            continue;
        }
        int dn = snprintf(s_paths[s_count], sizeof(s_paths[0]), "%s/", dir);
        if (dn > 0 && dn < (int)sizeof(s_paths[0])) {
            strlcpy(s_paths[s_count] + dn, ent->d_name, sizeof(s_paths[0]) - (size_t)dn);
        }
        gbk_to_utf8(ent->d_name, s_tracks[s_count], NAME_LEN);
        sanitize_name(s_tracks[s_count]);
        /* 去掉 .wav 后缀 */
        char *dot = strrchr(s_tracks[s_count], '.');
        if (dot) {
            *dot = '\0';
        }
        s_count++;
    }
    closedir(d);
}

static void rescan(void)
{
    s_count = 0;
    scan_dir("/sdcard/Music");
    if (s_count == 0) {
        scan_dir("/sdcard");
    }
    ESP_LOGI(TAG, "found %d wav file(s)", s_count);
}

/* ================= UI ================= */

static uint32_t cover_color(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h = (h ^ (uint8_t)*s++) * 16777619u;
    }
    static const uint32_t pal[6] = { 0x2B3A67, 0x3E2C5A, 0x1F4B4B, 0x5A2E3A, 0x2C3E50, 0x4A3B1F };
    return pal[h % 6];
}

/* ================= 歌词 ================= */
#define LRC_MAX 60
static char s_lrc[LRC_MAX][40];
static int  s_lrc_ms[LRC_MAX];
static volatile int  s_lrc_cnt;
static volatile bool s_lrc_busy;
static char s_lrc_kw[80];
static lv_obj_t *s_lyric_lbl[5];

static void lyric_request(const char *title, const char *artist);
static void trim_utf8_tail(char *s);

/* 当前时间对应的歌词行（最后一行 <= pos 的） */
static int lrc_index(int pos_ms)
{
    int idx = 0;
    for (int i = 0; i < s_lrc_cnt; i++) {
        if (s_lrc_ms[i] <= pos_ms) {
            idx = i;
        } else {
            break;
        }
    }
    return idx;
}

/* 中间高亮，上下各两行淡色 */
static void lyric_show(int pos_ms)
{
    if (!s_lyric_lbl[0]) {
        return;
    }
    int cnt = s_lrc_cnt;
    if (cnt <= 0) {
        for (int i = 0; i < 5; i++) {
            lv_label_set_text(s_lyric_lbl[i], i == 2 ? "暂无歌词" : "");
            lv_obj_set_style_text_color(s_lyric_lbl[i], col(0x44598A), 0);
        }
        return;
    }
    int idx = lrc_index(pos_ms);
    for (int i = 0; i < 5; i++) {
        int li = idx - 2 + i;
        if (li < 0 || li >= cnt) {
            lv_label_set_text(s_lyric_lbl[i], "");
            continue;
        }
        lv_label_set_text(s_lyric_lbl[i], ui_font_safe(s_lrc[li], &ui_cjk_16));
        lv_obj_set_style_text_color(s_lyric_lbl[i], col(i == 2 ? 0xFFFFFF : 0x5B6B85), 0);
    }
}

static void playlist_rebuild(void);

static void update_track_ui(void)
{
    if (s_count == 0) {
        if (s_lbl_empty) {
            lv_obj_remove_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(s_lbl_title, "还没有歌曲");
        lv_label_set_text(s_lbl_sub, "");
        return;
    }
    if (s_lbl_empty) {
        lv_obj_add_flag(s_lbl_empty, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_lbl_title, s_tracks[s_index]);
    lv_label_set_text_fmt(s_lbl_sub, "本地音乐 · 第 %d/%d 首", s_index + 1, s_count);
    /* 切歌后歌词要重新取（网络歌播放中就别覆盖了） */
    if (s_stream_url[0] == 0) {
        lyric_request(s_tracks[s_index], "");
    }
}

static void play_btn_cb(lv_event_t *e)
{
    (void)e;
    /* 网络歌曲 / 电台：暂停只置标志，连接保活，再按一次继续 */
    if (s_stream_url[0]) {
        if (s_paused) {
            s_paused = false;
        } else if (s_playing) {
            s_paused = true;
        } else {
            s_paused = false;
            s_cmd = 1;
            if (s_sem) xSemaphoreGive(s_sem);
        }
        return;
    }
    if (s_count == 0) {
        return;
    }
    if (s_playing) {
        s_cmd = 2;
        if (s_sem) xSemaphoreGive(s_sem);
    } else {
        play_index(s_index);
    }
}

static void prev_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;
    play_index((s_index + s_count - 1) % s_count);
}

static void next_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_count == 0) return;
    play_index((s_index + 1) % s_count);
}

static void pl_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!s_playlist) return;
    if (lv_obj_has_flag(s_playlist, LV_OBJ_FLAG_HIDDEN)) {
        playlist_rebuild();
        lv_obj_remove_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_playlist);
    } else {
        lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
    }
}

static void pl_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    play_index(idx);
    lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
}

static bool s_fav_only;      /* 播放列表只看收藏 */

/* ---- 网络电台搜索（radio-browser，免 key，直连 MP3 流） ---- */
#define NET_MAX 12
static char s_net_names[NET_MAX][44];
static char s_net_urls[NET_MAX][110];
static volatile int  s_net_cnt;
static volatile bool s_net_busy;
static char s_net_kw[32];

/* 前置声明（搜索历史/结果重建互相调用） */
static void search_rebuild(void);
static void hist_add(const char *kw);
static void dl_target_path(const char *name, char *out, size_t cap);
static void playlist_rebuild(void);
static int  s_src;          /* 0=酷我 1=网易云(免费曲目) 2=海外 Audius */

static void net_item_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_net_cnt) {
        return;
    }
    strlcpy((char *)s_stream_url, s_net_urls[i], sizeof(s_stream_url));
    s_cmd = 1;
    s_playing = false;
    s_paused = false;
    s_pos_ms = 0;
    s_total_ms = 0;
    strlcpy(s_cur_name, s_net_names[i], sizeof(s_cur_name));
    strlcpy(s_cur_url, s_net_urls[i], sizeof(s_cur_url));
    app_audio_out_flush();
    if (s_sem) xSemaphoreGive(s_sem);
    lv_label_set_text(s_lbl_title, s_net_names[i]);
    lv_label_set_text(s_lbl_sub, "在线播放");
    lyric_request(s_net_names[i], "");
    if (s_search) lv_obj_add_flag(s_search, LV_OBJ_FLAG_HIDDEN);
    ui_shell_toast("正在连接…");
}

/* UTF-8 关键字 → percent-encoded，搜索中文必须 */
static void url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < cap; p++) {
        unsigned char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = 0;
}

/* ================= 搜歌：酷我（国内曲库，免 key） ================= */

/* 一次 HTTP GET，读完整个 body（PSRAM）。返回字节数，<0 失败 */
static int http_get_body(const char *url, char *body, int cap, int timeout_ms)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = timeout_ms,
                                     .buffer_size = 4096,
                                     .user_agent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                     .crt_bundle_attach = esp_crt_bundle_attach };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return -1;
    }
    esp_http_client_set_header(cli, "Referer", "http://music.163.com/");
    int total = 0;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        int r;
        while (total < cap - 1 &&
               (r = esp_http_client_read(cli, body + total, cap - 1 - total)) > 0) {
            total += r;
        }
    } else {
        ESP_LOGW(TAG, "http open fail: %s", url);
    }
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    body[total] = 0;
    return total;
}

/* 酷我的播放地址要现取（有效期几分钟），返回真实 MP3 URL */
static bool kuwo_resolve(const char *rid, char *out, size_t cap)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://antiserver.kuwo.cn/anti.s?type=convert_url&format=mp3&response=url&rid=%s",
             rid);
    char *body = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    if (!body) {
        return false;
    }
    int n = http_get_body(url, body, 1024, 8000);
    bool ok = false;
    if (n > 0) {
        /* 去掉首尾空白和引号 */
        char *p = body;
        while (*p == ' ' || *p == '\r' || *p == '\n' || *p == '"') p++;
        char *e = p + strlen(p);
        while (e > p && (e[-1] == ' ' || e[-1] == '\r' || e[-1] == '\n' || e[-1] == '"')) {
            *--e = 0;
        }
        if (strncmp(p, "http", 4) == 0) {
            /* 统一走 http：没对时（RTC 未同步）也能放，省一次 TLS 握手 */
            if (strncmp(p, "https://", 8) == 0) {
                snprintf(out, cap, "http://%s", p + 8);
            } else {
                strlcpy(out, p, cap);
            }
            ok = true;
            ESP_LOGI(TAG, "kuwo %s -> %s", rid, out);
        } else {
            ESP_LOGW(TAG, "kuwo resolve failed: %.60s", p);
        }
    }
    heap_caps_free(body);
    return ok;
}

/* 粗判是否合法 UTF-8：酷我有时无视 encoding=utf8 直接回 GBK */
/* ================= 下载到 TF 卡 =================
 * 网络歌本身就有直链（酷我的先解析成 mp3 地址），下载只做两件事：
 * 取流 → 写 /sdcard/Music/xxx.mp3，完成后重新扫描，歌就变成"本地音乐"。
 */
static volatile bool s_dl_busy;
static volatile int  s_dl_result;      /* 0 无 1 成功 2 失败 */
static char s_dl_msg[96];
static char s_dl_url[128];
static char s_dl_name[NAME_LEN];
static volatile bool s_rescan_req;
static char s_dl_saved[64];      /* 实际保存路径（提示用） */

/* 文件名安全化：ASCII 里的非法字符替换成 '_'，中文原样保留 */
static void file_safe(char *s)
{
    for (char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c >= 0x80) {
            continue;
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ') {
            continue;
        }
        *p = '_';
    }
    /* 去掉尾部空格/点（FAT 不喜欢） */
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '.')) {
        s[--n] = 0;
    }
    if (n == 0) {
        strlcpy(s, "music", NAME_LEN);
    }
}

static void download_task(void *arg)
{
    (void)arg;
    char real[256] = "";
    const char *u = s_dl_url;

    if (strncmp(u, "kuwo://", 7) == 0) {
        if (!kuwo_resolve(u + 7, real, sizeof(real))) {
            strlcpy(s_dl_msg, "解析播放地址失败", sizeof(s_dl_msg));
            s_dl_result = 2;
            s_dl_busy = false;
            vTaskDeleteWithCaps(NULL);
            return;
        }
        u = real;
    }

    char path[220];
    dl_target_path(s_dl_name, path, sizeof(path));
    const char *slash = strrchr(path, '/');
    static char gbk[160];
    strlcpy(gbk, slash ? slash + 1 : path, sizeof(gbk));

    int mk = mkdir("/sdcard/Music", 0777);
    int mk_err = errno;
    FILE *f = fopen(path, "wb");
    int op_err = errno;
    if (!f) {
        /* 目录建不出来（有些卡根目录只读/异常）就退回 /sdcard 根目录 */
        ESP_LOGW(TAG, "fopen %s failed (errno=%d), mkdir=%d/%d", path, op_err, mk, mk_err);
        snprintf(path, sizeof(path), "/sdcard/%s.mp3", gbk);
        f = fopen(path, "wb");
        op_err = errno;
    }
    if (!f) {
        ESP_LOGE(TAG, "download: cannot create file, errno=%d", op_err);
        snprintf(s_dl_msg, sizeof(s_dl_msg), "写文件失败 errno=%d", op_err);
        s_dl_result = 2;
        s_dl_busy = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    ESP_LOGI(TAG, "download -> %s", path);
    strlcpy(s_dl_saved, path, sizeof(s_dl_saved));

    char *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        strlcpy(s_dl_msg, "内存不足", sizeof(s_dl_msg));
        s_dl_result = 2;
        s_dl_busy = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    esp_http_client_config_t cfg = { .url = u, .timeout_ms = 20000, .buffer_size = 4096,
                                     .user_agent = "Mozilla/5.0" };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    long total = 0;
    if (cli && esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        int r;
        while ((r = esp_http_client_read(cli, buf, 8192)) > 0) {
            if (fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
                break;
            }
            total += r;
            if (total > 30 * 1024 * 1024) {
                break;                  /* 防止异常大文件把卡写满 */
            }
        }
        esp_http_client_close(cli);
    }
    if (cli) {
        esp_http_client_cleanup(cli);
    }
    fclose(f);
    heap_caps_free(buf);

    if (total < 8192) {
        remove(path);                   /* 只有几 KB，基本是错误页 */
        strlcpy(s_dl_msg, "下载失败（网络或版权限制）", sizeof(s_dl_msg));
        s_dl_result = 2;
    } else {
        ESP_LOGI(TAG, "downloaded %ld bytes -> %s", total, path);
        /* 明确告诉用户存到哪了。文件名是 GBK 字节，不能直接塞进提示（会乱码），
         * 只说目录 */
        bool in_music = (strstr(s_dl_saved, "/Music/") != NULL);
        snprintf(s_dl_msg, sizeof(s_dl_msg), "%ldKB 已存到 %s",
                 total / 1024, in_music ? "TF卡/Music" : "TF卡根目录");
        s_dl_result = 1;
        s_rescan_req = true;
    }
    s_dl_busy = false;
    vTaskDeleteWithCaps(NULL);
}

/* 下载目标路径（FATFS 是 GBK 代码页，名字要转码） */
static void dl_target_path(const char *name, char *out, size_t cap)
{
    char fn[NAME_LEN + 8];
    strlcpy(fn, name, sizeof(fn));
    file_safe(fn);
    static char gbk[160];
    utf8_to_gbk(fn, gbk, sizeof(gbk));
    snprintf(out, cap, "/sdcard/Music/%s.mp3", gbk);
}

/* 本地曲目里是否已经有这首（按去掉扩展名的名字比较） */
static int find_local_by_name(const char *name)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_tracks[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool dl_exists(const char *name)
{
    char path[220];
    dl_target_path(name, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 8192) {
        return true;
    }
    /* 退回根目录那种情况也查一下 */
    char alt[220];
    const char *base = strrchr(path, '/');
    if (base) {
        snprintf(alt, sizeof(alt), "/sdcard/%s", base + 1);
        if (stat(alt, &st) == 0 && st.st_size > 8192) {
            return true;
        }
    }
    return find_local_by_name(name) >= 0;
}

static void dl_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_dl_busy) {
        ui_shell_toast("正在下载，稍等…");
        return;
    }
    if (!s_cur_name[0] || !s_cur_url[0]) {
        ui_shell_toast("只有网络歌曲能下载");
        return;
    }
    if (!app_storage_is_sd()) {
        ui_shell_toast("没检测到 TF 卡，无法下载");
        return;
    }
    if (dl_exists(s_cur_name)) {
        ui_shell_toast("已在本地，不用重复下载");
        return;
    }
    strlcpy(s_dl_url, s_cur_url, sizeof(s_dl_url));
    strlcpy(s_dl_name, s_cur_name, sizeof(s_dl_name));
    s_dl_result = 0;
    s_dl_busy = true;
    if (xTaskCreateWithCaps(download_task, "dl", 12288, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_dl_busy = false;
        ui_shell_toast("任务创建失败");
    } else {
        ui_shell_toast("开始下载到 TF 卡…");
    }
}

static bool utf8_valid(const char *s, int n)
{
    const unsigned char *p = (const unsigned char *)s;
    int i = 0;
    while (i < n && p[i]) {
        unsigned char c = p[i];
        int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                  ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 0;
        if (len == 0 || i + len > n) {
            return false;
        }
        for (int k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

/* 字段级编码修正：不是合法 UTF-8 就当 GBK 转一下（酷我常常回 GBK） */
static void field_to_utf8(char *s, size_t cap)
{
    if (!s[0] || utf8_valid(s, (int)strlen(s))) {
        return;
    }
    static char tmp[256];       /* 只在搜索任务里用，串行调用，不用每次分配 */
    if (cap * 2 > sizeof(tmp)) {
        return;
    }
    if (gbk_to_utf8(s, tmp, sizeof(tmp)) > 0) {
        size_t use = strlen(tmp);
        if (use > cap - 1) {
            use = cap - 1;
            while (use > 0 && ((unsigned char)tmp[use] & 0xC0) == 0x80) {
                use--;      /* 不切断多字节字符 */
            }
        }
        memcpy(s, tmp, use);
        s[use] = 0;
    }
}

/* 截断到最后一个完整 UTF-8 字符（避免尾部半个汉字显示成方框） */
static void trim_utf8_tail(char *s)
{
    size_t n = strlen(s);
    size_t i = 0, last = 0;
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int len = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0) ? 2 :
                  ((c & 0xF0) == 0xE0) ? 3 : 4;
        if (i + (size_t)len > n) {
            s[last] = 0;
            return;
        }
        i += (size_t)len;
        last = i;
    }
}

/* 把 "&nbsp;" 这类实体压成一个空格 */
static void replace_entity(char *s, const char *ent)
{
    size_t el = strlen(ent);
    char *q;
    while ((q = strstr(s, ent)) != NULL) {
        *q = ' ';
        memmove(q + 1, q + el, strlen(q + el) + 1);
    }
}

/* 解析酷我搜索返回（单引号 + &nbsp; 实体） */
static int parse_kuwo(char *body, int n_start)
{
    int n = n_start;
    char *p = body;
    while (n < NET_MAX) {
        char *a = strstr(p, "'ARTIST':'");
        if (!a) {
            break;
        }
        a += 10;
        char *ae = strchr(a, '\'');
        if (!ae) {
            break;
        }
        char *m = strstr(ae, "'MUSICRID':'");
        if (!m) {
            break;
        }
        m += 12;
        char *me = strchr(m, '\'');
        if (!me) {
            break;
        }
        char *nm = strstr(me, "'NAME':'");
        if (!nm) {
            break;
        }
        nm += 8;
        char *ne = strchr(nm, '\'');
        if (!ne) {
            break;
        }

        char name[80], artist[48];
        size_t nl = (size_t)(ne - nm);
        if (nl >= sizeof(name)) nl = sizeof(name) - 1;
        memcpy(name, nm, nl);
        name[nl] = 0;
        size_t al = (size_t)(ae - a);
        if (al >= sizeof(artist)) al = sizeof(artist) - 1;
        memcpy(artist, a, al);
        artist[al] = 0;

        field_to_utf8(name, sizeof(name));
        field_to_utf8(artist, sizeof(artist));
        /* 酷我用 &nbsp; 当空格（删 6 字节里的后 5 个，保留一个空格） */
        replace_entity(name, "&nbsp;");
        replace_entity(artist, "&nbsp;");

        char rid[24];
        size_t rl = (size_t)(me - m);
        if (rl >= sizeof(rid)) rl = sizeof(rid) - 1;
        memcpy(rid, m, rl);
        rid[rl] = 0;

        if (name[0]) {
            snprintf(s_net_names[n], sizeof(s_net_names[0]), "%.24s%s%.10s",
                     name, artist[0] ? " - " : "", artist);
            trim_utf8_tail(s_net_names[n]);
            sanitize_name(s_net_names[n]);
            snprintf(s_net_urls[n], sizeof(s_net_urls[0]), "kuwo://%s", rid);
            n++;
        }
        p = ne;
    }
    return n;
}

/* 解析 Audius 搜索返回（海外曲库，兜底用） */
static int parse_audius(char *body, int total, int n_start)
{
    int n = n_start;
    const char *p = body;
    while (n < NET_MAX && (p = strstr(p, "\"track_id\":")) != NULL) {
        const char *nxt = strstr(p + 11, "\"track_id\":");
        const char *endp = nxt ? nxt : body + total;
        const char *q = strstr(p, "\"id\":\"");
        if (q && q < endp) {
            q += 6;
            const char *e = strchr(q, '"');
            if (e && e < endp && (e - q) < 20) {
                char sid[20];
                size_t l = (size_t)(e - q);
                memcpy(sid, q, l);
                sid[l] = 0;

                char title[40] = "Audius";
                const char *t = strstr(p, "\"title\":\"");
                if (t && t < endp) {
                    t += 9;
                    const char *te = strchr(t, '"');
                    if (te && te < endp) {
                        size_t tl = (size_t)(te - t);
                        if (tl > 38) tl = 38;
                        memcpy(title, t, tl);
                        title[tl] = 0;
                    }
                }
                char artist[22] = "";
                const char *u = strstr(p, "\"user\":{");
                if (u && u < endp) {
                    const char *nm = strstr(u, "\"name\":\"");
                    if (nm && nm < endp) {
                        nm += 8;
                        const char *ne = strchr(nm, '"');
                        if (ne && ne < endp) {
                            size_t al = (size_t)(ne - nm);
                            if (al > 18) al = 18;
                            memcpy(artist, nm, al);
                            artist[al] = 0;
                        }
                    }
                }
                snprintf(s_net_names[n], sizeof(s_net_names[0]), "%.24s%s%.10s",
                         title, artist[0] ? " - " : "", artist);
                trim_utf8_tail(s_net_names[n]);
            sanitize_name(s_net_names[n]);
                snprintf(s_net_urls[n], sizeof(s_net_urls[0]),
                         "https://discoveryprovider.audius.co/v1/tracks/%s/stream?app_name=ESP32P4", sid);
                n++;
            }
        }
        p = endp;
    }
    return n;
}

/* 解析网易云搜索（只保留免费曲目 fee==0/8，VIP 的放不了） */
static int parse_netease(char *body, int n_start)
{
    int n = n_start;
    char *p = body;
    while (n < NET_MAX) {
        char *a = strstr(p, "\"album\":{");
        if (!a) {
            break;
        }
        char *nxt = strstr(a + 1, "\"album\":{");
        char *end = nxt ? nxt : body + strlen(body);

        char *fee_p = strstr(a, "\"fee\":");
        int fee = fee_p && fee_p < end ? atoi(fee_p + 6) : 1;

        char *mv = strstr(a, "\"mvid\":");
        char *nm = (mv && mv < end) ? strstr(mv, "\"name\":\"") : NULL;
        char *idp = nm ? strstr(nm, "\"id\":") : NULL;
        if (!nm || !idp || nm > end || idp > end) {
            p = end;
            continue;
        }
        char *ar = strstr(a, "\"artists\":[{\"");
        char *an = (ar && ar < end) ? strstr(ar, "\"name\":\"") : NULL;

        if (fee == 0 || fee == 8) {
            nm += 8;
            char *ne = strchr(nm, '"');
            char title[40] = "";
            if (ne) {
                size_t l = (size_t)(ne - nm);
                if (l >= sizeof(title)) l = sizeof(title) - 1;
                memcpy(title, nm, l);
                title[l] = 0;
            }
            char artist[28] = "";
            if (an) {
                an += 8;
                char *ae = strchr(an, '"');
                if (ae) {
                    size_t l = (size_t)(ae - an);
                    if (l >= sizeof(artist)) l = sizeof(artist) - 1;
                    memcpy(artist, an, l);
                    artist[l] = 0;
                }
            }
            if (title[0]) {
                long id = atol(idp + 5);
                snprintf(s_net_names[n], sizeof(s_net_names[0]), "%.24s%s%.10s",
                         title, artist[0] ? " - " : "", artist);
                trim_utf8_tail(s_net_names[n]);
                sanitize_name(s_net_names[n]);
                snprintf(s_net_urls[n], sizeof(s_net_urls[0]),
                         "http://music.163.com/song/media/outer/url?id=%ld.mp3", id);
                n++;
            }
        }
        p = end;
    }
    return n;
}

/* 解析 radio-browser 电台列表 */
static int parse_radio(char *body, int n_start)
{
    int n = n_start;
    const char *p = body;
    while (n < NET_MAX && (p = strstr(p, "\"name\":\"")) != NULL) {
        p += 8;
        const char *e = strchr(p, '"');
        if (!e) break;
        size_t len = (size_t)(e - p);
        if (len >= sizeof(s_net_names[0])) len = sizeof(s_net_names[0]) - 1;
        memcpy(s_net_names[n], p, len);
        s_net_names[n][len] = 0;
        sanitize_name(s_net_names[n]);
        p = e;

        const char *u = strstr(p, "\"url_resolved\":\"");
        if (!u) break;
        u += 16;
        const char *ue = strchr(u, '"');
        if (!ue) break;
        len = (size_t)(ue - u);
        if (len >= sizeof(s_net_urls[0])) len = sizeof(s_net_urls[0]) - 1;
        memcpy(s_net_urls[n], u, len);
        s_net_urls[n][len] = 0;
        p = ue;
        if (strncmp(s_net_urls[n], "http", 4) == 0) {
            n++;
        }
    }
    return n;
}

#define NET_BODY_SIZE (24 * 1024)

/* ---- 歌词抓取：网易云搜索 + 歌词接口（免登录，HTTP） ---- */

static void lrc_parse(const char *body)
{
    const char *p = strstr(body, "\"lyric\":\"");
    if (!p) {
        return;
    }
    p += 9;
    const int cap = 4096;
    char *txt = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!txt) {
        return;
    }
    size_t o = 0;
    while (*p && o + 1 < (size_t)cap) {
        if (*p == '\\') {
            char e = p[1];
            if (e == 'n') {
                txt[o++] = '\n';
                p += 2;
            } else if (e == 'r') {
                p += 2;
            } else if (e == '"' || e == '\\') {
                txt[o++] = e;
                p += 2;
            } else if (e == 'u') {
                p += 6;                 /* 跳过 \uXXXX */
            } else {
                txt[o++] = e;
                p += 2;
            }
            continue;
        }
        if (*p == '"') {
            break;
        }
        txt[o++] = *p++;
    }
    txt[o] = 0;

    s_lrc_cnt = 0;
    char *save = NULL;
    for (char *line = strtok_r(txt, "\n", &save); line && s_lrc_cnt < LRC_MAX;
         line = strtok_r(NULL, "\n", &save)) {
        if (line[0] != '[') {
            continue;
        }
        char *rb = strchr(line, ']');
        if (!rb) {
            continue;
        }
        int mm = 0, ss = 0, frac = 0;
        if (sscanf(line + 1, "%d:%d.%d", &mm, &ss, &frac) != 3) {
            continue;                   /* [by:xxx] 之类直接跳过 */
        }
        /* 小数位数：2 位是厘秒，3 位是毫秒 */
        int digits = 0;
        for (const char *d = line; *d && *d != ']'; d++) {
            if (*d == '.') {
                digits = 0;
                for (const char *q = d + 1; *q >= '0' && *q <= '9'; q++) {
                    digits++;
                }
                break;
            }
        }
        if (digits == 2) {
            frac *= 10;
        } else if (digits == 1) {
            frac *= 100;
        } else if (digits > 3) {
            for (int k = digits; k > 3; k--) {
                frac /= 10;
            }
        }
        int t = mm * 60000 + ss * 1000 + frac;
        if (ss > 59) {
            continue;
        }
        const char *tx = rb + 1;
        while (*tx == ' ') {
            tx++;
        }
        if (!*tx) {
            continue;
        }
        if (s_lrc_cnt > 0 && t <= s_lrc_ms[s_lrc_cnt - 1]) {
            continue;                   /* 时间倒序/重复行丢掉 */
        }
        s_lrc_ms[s_lrc_cnt] = t;
        strlcpy(s_lrc[s_lrc_cnt], tx, sizeof(s_lrc[0]));
        trim_utf8_tail(s_lrc[s_lrc_cnt]);
        s_lrc_cnt++;
    }
    heap_caps_free(txt);
    if (s_lrc_cnt == 1 && strstr(s_lrc[0], "暂无歌词")) {
        s_lrc_cnt = 0;
    }
}

static void lyric_task(void *arg)
{
    char kw[80];
    strlcpy(kw, s_lrc_kw, sizeof(kw));
    char *body = heap_caps_malloc(16 * 1024, MALLOC_CAP_SPIRAM);
    if (!body) {
        s_lrc_busy = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    char kw_enc[260];
    url_encode(kw, kw_enc, sizeof(kw_enc));
    char url[380];
    snprintf(url, sizeof(url),
             "http://music.163.com/api/search/get/web?s=%s&type=1&limit=1&offset=0", kw_enc);
    long id = 0;
    int n = http_get_body(url, body, 16 * 1024, 10000);
    if (n > 0) {
        /* 注意：album/artist 里也有 id，歌曲自己的 id 在 song 级字段 mvid 之后，
         * 用 mvid 做锚点最稳（album 里没有 mvid） */
        const char *sp = strstr(body, "\"songs\"");
        const char *cp = sp ? strstr(sp, "\"mvid\":") : NULL;
        const char *ip = cp ? strstr(cp, "\"id\":") : NULL;
        if (ip) {
            id = atol(ip + 5);
        }
    }
    if (id > 0) {
        snprintf(url, sizeof(url),
                 "http://music.163.com/api/song/lyric?os=pc&id=%ld&lv=-1&kv=-1&tv=-1", id);
        n = http_get_body(url, body, 16 * 1024, 10000);
        if (n > 0) {
            lrc_parse(body);
        }
    }
    heap_caps_free(body);
    s_lrc_busy = false;
    ESP_LOGI(TAG, "lyric \"%s\" id=%ld lines=%d", s_lrc_kw, id, s_lrc_cnt);
    vTaskDeleteWithCaps(NULL);
}

static void lyric_request(const char *title, const char *artist)
{
    if (!title || !title[0]) {
        return;
    }
    char kw[80];
    snprintf(kw, sizeof(kw), "%s%s%s", title,
             (artist && artist[0]) ? " " : "", artist ? artist : "");
    if (strcmp(kw, s_lrc_kw) == 0 && s_lrc_cnt > 0) {
        return;                         /* 同一首，不用重复取 */
    }
    strlcpy(s_lrc_kw, kw, sizeof(s_lrc_kw));
    s_lrc_cnt = 0;
    if (s_lrc_busy) {
        return;
    }
    s_lrc_busy = true;
    if (xTaskCreateWithCaps(lyric_task, "lyric", 8192, NULL, 2, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_lrc_busy = false;
    }
}

static void net_search_task(void *arg)
{
    int mode = (int)(intptr_t)arg;      /* 0=电台 1=仅海外 2=国内歌曲(酷我) */
    char kw_enc[160];
    url_encode(s_net_kw, kw_enc, sizeof(kw_enc));
    char url[360];
    char *body = heap_caps_malloc(NET_BODY_SIZE, MALLOC_CAP_SPIRAM);
    if (!body) {
        s_net_busy = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }
    if (s_net_kw[0] == 0) {
        strlcpy(s_net_kw, "热门", sizeof(s_net_kw));
        url_encode(s_net_kw, kw_enc, sizeof(kw_enc));
    }
    ESP_LOGI(TAG, "net_search start mode=%d kw=%s", mode, s_net_kw);

    int n = 0;
    if (mode == 2) {
        /* 国内曲库：酷我，中文歌基本都能搜到 */
        snprintf(url, sizeof(url),
                 "http://search.kuwo.cn/r.s?all=%s&ft=music&itemset=web_2013&client=kt"
                 "&pn=0&rn=%d&rformat=json&encoding=utf8", kw_enc, NET_MAX);
        int total = http_get_body(url, body, NET_BODY_SIZE, 12000);
        ESP_LOGI(TAG, "kuwo search bytes=%d", total);
        if (total > 0) {
            n = parse_kuwo(body, 0);
        }
        if (n == 0) {
            /* 国内没结果 → 退到 Audius 兜底 */
            snprintf(url, sizeof(url),
                     "https://discoveryprovider.audius.co/v1/tracks/search?query=%s"
                     "&app_name=ESP32P4&limit=%d", kw_enc, NET_MAX);
            total = http_get_body(url, body, NET_BODY_SIZE, 12000);
            if (total > 0) {
                n = parse_audius(body, total, 0);
            }
        }
    } else if (mode == 3) {
        /* 网易云（免费曲目可完整播放） */
        snprintf(url, sizeof(url),
                 "http://music.163.com/api/search/get/web?s=%s&type=1&limit=%d&offset=0",
                 kw_enc, NET_MAX);
        int total = http_get_body(url, body, NET_BODY_SIZE, 12000);
        ESP_LOGI(TAG, "netease search bytes=%d", total);
        if (total > 0) {
            n = parse_netease(body, 0);
        }
    } else if (mode == 1) {
        snprintf(url, sizeof(url),
                 "https://discoveryprovider.audius.co/v1/tracks/search?query=%s"
                 "&app_name=ESP32P4&limit=%d", kw_enc, NET_MAX);
        int total = http_get_body(url, body, NET_BODY_SIZE, 12000);
        if (total > 0) {
            n = parse_audius(body, total, 0);
        }
    } else {
        snprintf(url, sizeof(url),
                 "http://de1.api.radio-browser.info/json/stations/search?name=%s&codec=MP3"
                 "&limit=%d&hidebroken=true&order=clickcount&reverse=true", kw_enc, NET_MAX);
        int total = http_get_body(url, body, NET_BODY_SIZE, 12000);
        if (total > 0) {
            n = parse_radio(body, 0);
        }
    }

    heap_caps_free(body);
    s_net_cnt = n;
    s_net_busy = false;
    ESP_LOGI(TAG, "net search kw=%s hits=%d", s_net_kw, n);
    vTaskDeleteWithCaps(NULL);
}

/* 「搜歌」：国内曲库优先（酷我），海外 Audius 兜底 */
static void song_search_cb(lv_event_t *e)
{
    (void)e;
    if (s_net_busy) {
        ui_shell_toast("正在搜索…");
        return;
    }
    if (s_search_kw[0] == 0) {
        ui_shell_toast("先输入歌名或歌手，如：天外来物");
        return;
    }
    strlcpy(s_net_kw, s_search_kw, sizeof(s_net_kw));
    hist_add(s_search_kw);
    s_net_cnt = 0;
    s_net_busy = true;
    int mode = (s_src == 1) ? 3 : (s_src == 2) ? 1 : 2;
    ui_shell_toast(s_src == 1 ? "正在搜索（网易云免费曲目）…" :
                   s_src == 2 ? "正在搜索（海外 Audius）…" : "正在搜索（酷我）…");
    xTaskCreateWithCaps(net_search_task, "net_srch", 12288, (void *)mode, 3, NULL, MALLOC_CAP_SPIRAM);
}

static void net_search_cb(lv_event_t *e)
{
    (void)e;
    if (s_net_busy) {
        ui_shell_toast("正在搜索…");
        return;
    }
    if (s_search_kw[0] == 0) {
        ui_shell_toast("先输入关键字（英文更容易搜到）");
        return;
    }
    strlcpy(s_net_kw, s_search_kw, sizeof(s_net_kw));
    hist_add(s_search_kw);
    s_net_cnt = 0;
    s_net_busy = true;
    ui_shell_toast("正在搜索电台…");
    xTaskCreateWithCaps(net_search_task, "net_srch", 12288, (void *)0, 3, NULL, MALLOC_CAP_SPIRAM);
}

/* ---- 搜索 ---- */
static volatile bool s_search_dirty;

/* 键盘每次输入只置脏标志，真正重建放到 tick 里（节流，避免对象风暴） */
static void search_mark_dirty(void)
{
    s_search_dirty = true;
}

/* ---- 搜索历史（最多 4 条，可单条删除） ---- */
#define HIST_MAX 4
static char s_hist[HIST_MAX][32];
static int  s_hist_cnt;

static void hist_save(void)
{
    char buf[160] = { 0 };
    size_t o = 0;
    for (int i = 0; i < s_hist_cnt && o + 34 < sizeof(buf); i++) {
        o += (size_t)snprintf(buf + o, sizeof(buf) - o, "%s%s",
                              i ? "\n" : "", s_hist[i]);
    }
    nvs_handle_t h;
    if (nvs_open("music", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "hist", buf);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void hist_load(void)
{
    s_hist_cnt = 0;
    char buf[160] = { 0 };
    size_t len = sizeof(buf);
    nvs_handle_t h;
    if (nvs_open("music", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, "hist", buf, &len);
        nvs_close(h);
    }
    char *save = NULL;
    for (char *t = strtok_r(buf, "\n", &save); t && s_hist_cnt < HIST_MAX;
         t = strtok_r(NULL, "\n", &save)) {
        if (t[0]) {
            strlcpy(s_hist[s_hist_cnt++], t, sizeof(s_hist[0]));
        }
    }
}

static void hist_add(const char *kw)
{
    if (!kw || !kw[0]) {
        return;
    }
    int found = -1;
    for (int i = 0; i < s_hist_cnt; i++) {
        if (strcmp(s_hist[i], kw) == 0) {
            found = i;
            break;
        }
    }
    if (found == 0) {
        return;                         /* 已经是最新 */
    }
    int keep = s_hist_cnt;
    if (found > 0) {
        for (int k = found; k < s_hist_cnt - 1; k++) {
            strlcpy(s_hist[k], s_hist[k + 1], sizeof(s_hist[0]));
        }
        keep = s_hist_cnt - 1;
    } else if (s_hist_cnt < HIST_MAX) {
        keep = s_hist_cnt;
    } else {
        keep = HIST_MAX - 1;
    }
    /* 整体后移一格，插到最前 */
    for (int k = keep; k > 0; k--) {
        strlcpy(s_hist[k], s_hist[k - 1], sizeof(s_hist[0]));
    }
    strlcpy(s_hist[0], kw, sizeof(s_hist[0]));
    s_hist_cnt = keep + 1;
    if (s_hist_cnt > HIST_MAX) {
        s_hist_cnt = HIST_MAX;
    }
    hist_save();
}

static void hist_del(int idx)
{
    if (idx < 0 || idx >= s_hist_cnt) {
        return;
    }
    for (int k = idx; k < s_hist_cnt - 1; k++) {
        strlcpy(s_hist[k], s_hist[k + 1], sizeof(s_hist[0]));
    }
    s_hist_cnt--;
    memset(s_hist[s_hist_cnt], 0, sizeof(s_hist[0]));
    hist_save();
}

static void hist_pick_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_hist_cnt) {
        return;
    }
    strlcpy(s_search_kw, s_hist[idx], sizeof(s_search_kw));
    if (s_search_ta) {
        lv_label_set_text(s_search_ta, s_search_kw);
    }
    hist_add(s_hist[idx]);              /* 提到最前 */
    search_rebuild();
    ui_shell_toast("已填入，点「搜歌」或「搜网络」");
}

static void hist_del_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    hist_del(idx);
    search_rebuild();
}

/* 关键词为空时，列表区显示搜索历史 */
static void search_show_history(void)
{
    lv_obj_clean(s_search_list);
    if (s_hist_cnt == 0) {
        lv_obj_t *l = ui_label(s_search_list, "输入歌名 / 歌手，或用「搜网络」", &ui_cjk_16, 0x7B8CA6);
        lv_obj_set_pos(l, 8, 10);
        return;
    }
    for (int i = 0; i < s_hist_cnt; i++) {
        lv_obj_t *row = lv_button_create(s_search_list);
        lv_obj_set_size(row, 424, 32);
        lv_obj_set_style_bg_color(row, col(0x141A24), 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_add_event_cb(row, hist_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(row, s_hist[i], &ui_cjk_16, 0xC7E8FF);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_t *del = lv_button_create(row);
        lv_obj_set_size(del, 30, 26);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 4, 0);
        lv_obj_set_style_bg_color(del, col(0x3A2030), 0);
        lv_obj_set_style_radius(del, 8, 0);
        lv_obj_set_style_pad_all(del, 0, 0);
        lv_obj_add_event_cb(del, hist_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *dl = ui_label(del, "×", &lv_font_montserrat_16, 0xFF8A8A);
        lv_obj_center(dl);
    }
}

static void search_rebuild(void)
{
    if (!s_search_list || !s_search_ta) {
        return;
    }
    if (!s_search_kw[0]) {
        search_show_history();
        return;
    }
    lv_obj_clean(s_search_list);
    const char *kw = s_search_kw;
    int hit = 0;
    for (int i = 0; i < s_count && hit < 6; i++) {
        if (kw[0] && !strstr(s_tracks[i], kw)) {
            continue;
        }
        lv_obj_t *row = lv_button_create(s_search_list);
        lv_obj_set_size(row, 440, 42);
        lv_obj_set_style_bg_color(row, col(i == s_index ? 0x1F2A3D : 0x141A24), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_add_event_cb(row, pl_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(row, s_tracks[i], &ui_cjk_16, C_TXT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
        hit++;
    }
    /* 网络电台结果 */
    for (int i = 0; i < s_net_cnt && i < NET_MAX; i++) {
        lv_obj_t *row = lv_button_create(s_search_list);
        lv_obj_set_size(row, 440, 42);
        lv_obj_set_style_bg_color(row, col(0x14202A), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_add_event_cb(row, net_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(row, s_net_names[i], &ui_cjk_16, 0xC7E8FF);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);
        lv_obj_t *tag = ui_label(row, "在线", &ui_cjk_16, 0x7BD6A6);
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, -8, 0);
        hit++;
    }
    if (hit == 0) {
        lv_obj_t *l = ui_label(s_search_list, s_net_busy ? "正在搜索网络…" : "没有结果，可点「搜网络」",
                               &ui_cjk_16, 0x7B8CA6);
        lv_obj_set_pos(l, 8, 10);
    }
}

static void search_ta_cb(lv_event_t *e)
{
    (void)e;
    search_rebuild();
}

static lv_obj_t *s_lbl_src;

static void src_btn_refresh(void)
{
    if (!s_lbl_src) {
        return;
    }
    lv_label_set_text(s_lbl_src, s_src == 1 ? "源 网易" : s_src == 2 ? "源 海外" : "源 酷我");
}

static void src_btn_cb(lv_event_t *e)
{
    (void)e;
    s_src = (s_src + 1) % 3;
    src_btn_refresh();
    s_net_cnt = 0;
    s_search_dirty = true;
    ui_shell_toast(s_src == 1 ? "网易云：仅免费曲目可完整播放" :
                   s_src == 2 ? "海外 Audius：独立音乐" : "酷我：曲库全，VIP 歌只有试听");
}

static void search_close_cb(lv_event_t *e)
{
    (void)e;
    if (s_search) lv_obj_add_flag(s_search, LV_OBJ_FLAG_HIDDEN);
}

static void search_open_cb(lv_event_t *e)
{
    (void)e;
    if (!s_search) return;
    s_search_kw[0] = 0;
    s_net_cnt = 0;
    lv_label_set_text(s_search_ta, "");
    ui_keypad_bind(s_search_kw, sizeof(s_search_kw), s_search_ta, search_mark_dirty, NULL);
    search_rebuild();
    lv_obj_remove_flag(s_search, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_search);
}

static void play_stream_now(const char *name, const char *url)
{
    strlcpy((char *)s_stream_url, url, sizeof(s_stream_url));
    s_cmd = 1;
    s_playing = false;
    s_paused = false;
    s_pos_ms = 0;
    s_total_ms = 0;
    strlcpy(s_cur_name, name, sizeof(s_cur_name));
    strlcpy(s_cur_url, url, sizeof(s_cur_url));
    app_audio_out_flush();
    if (s_sem) {
        xSemaphoreGive(s_sem);
    }
    if (s_lbl_title) {
        lv_label_set_text(s_lbl_title, name);
        lv_label_set_text(s_lbl_sub, "在线播放");
    }
    lyric_request(name, "");
    ui_shell_toast("正在连接…");
}

static void fav_item_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_fav_cnt) {
        return;
    }
    if (s_fav_url[i][0]) {
        play_stream_now(s_fav_name[i], s_fav_url[i]);
    } else {
        for (int k = 0; k < s_count; k++) {
            if (strcmp(s_tracks[k], s_fav_name[i]) == 0) {
                play_index(k);
                update_track_ui();
                break;
            }
        }
    }
    if (s_playlist) {
        lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
    }
}

static void fav_btn_cb(lv_event_t *e)
{
    (void)e;
    if (!s_cur_name[0]) {
        ui_shell_toast("先播放一首歌");
        return;
    }
    bool on = fav_toggle(s_cur_name, s_cur_url);
    if (s_lbl_fav) {
        lv_label_set_text(s_lbl_fav, on ? "已收藏" : "收藏");
        lv_obj_t *btn = lv_obj_get_parent(s_lbl_fav);
        if (btn) {
            lv_obj_set_style_bg_color(btn, col(on ? 0xB0304A : 0x1A2230), 0);
        }
    }
    if (s_playlist && !lv_obj_has_flag(s_playlist, LV_OBJ_FLAG_HIDDEN) && s_fav_only) {
        playlist_rebuild();
    }
    ui_shell_toast(on ? "已加入我喜欢" : "已取消收藏");
}

static void fav_list_cb(lv_event_t *e)
{
    (void)e;
    s_fav_only = !s_fav_only;
    playlist_rebuild();
}

/* 删除本地曲目（含下载下来的 MP3） */
static void pl_del_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_count || !s_paths[i][0]) {
        return;
    }
    char path[220];
    strlcpy(path, s_paths[i], sizeof(path));

    if (s_playing && i == s_index) {      /* 正在放这首：先停 */
        s_cmd = 3;
        s_playing = false;
        s_paused = false;
        if (s_sem) {
            xSemaphoreGive(s_sem);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (remove(path) != 0) {
        ESP_LOGE(TAG, "delete %s failed (errno=%d)", path, errno);
        ui_shell_toast("删除失败");
        return;
    }
    ESP_LOGI(TAG, "deleted %s", path);
    ui_shell_toast("已从 TF 卡删除");
    rescan();
    if (s_index >= s_count) {
        s_index = 0;
    }
    playlist_rebuild();
    update_track_ui();
}

static void playlist_rebuild(void)
{
    if (!s_plist) return;
    lv_obj_t *list = s_plist;
    lv_obj_clean(list);

    if (s_fav_only) {
        /* 收藏里既有网络歌（存了网址），也有本地文件（按名字找回） */
        for (int i = 0; i < s_fav_cnt; i++) {
            lv_obj_t *row = lv_button_create(list);
            lv_obj_set_size(row, 424, 42);
            lv_obj_set_style_bg_color(row, col(0x2A1B24), 0);
            lv_obj_set_style_radius(row, 10, 0);
            lv_obj_add_event_cb(row, fav_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_t *l = ui_label(row, s_fav_name[i], &ui_cjk_16, C_TXT);
            lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);
            if (s_fav_url[i][0]) {
                lv_obj_t *tg = ui_label(row, "在线", &ui_cjk_16, 0x7BD6A6);
                lv_obj_align(tg, LV_ALIGN_RIGHT_MID, -8, 0);
            }
        }
        return;
    }

    for (int i = 0; i < s_count; i++) {
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_size(row, 424, 42);
        lv_obj_set_style_bg_color(row, col(i == s_index ? 0x1F2A3D : 0x141A24), 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_add_event_cb(row, pl_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = ui_label(row, s_tracks[i], &ui_cjk_16, C_TXT);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 4, 0);

        /* 本地曲目可以直接删（下载下来的也在这里） */
        lv_obj_t *del = lv_button_create(row);
        lv_obj_set_size(del, 34, 30);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, 2, 0);
        lv_obj_set_style_bg_color(del, col(0x6B2038), 0);
        lv_obj_set_style_radius(del, 8, 0);
        lv_obj_set_style_pad_all(del, 0, 0);
        lv_obj_add_event_cb(del, pl_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *dl = ui_label(del, "删", &ui_cjk_16, 0xFFB0B0);
        lv_obj_center(dl);
    }
}

/* 唱片中心画个音符（用图形拼，避免字体缺字形） */
static void build_note(lv_obj_t *parent)
{
    lv_obj_t *head1 = lv_obj_create(parent);
    lv_obj_set_size(head1, 34, 26);
    lv_obj_set_pos(head1, 74, 108);
    lv_obj_set_style_radius(head1, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head1, col(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(head1, 235, 0);
    lv_obj_set_style_border_width(head1, 0, 0);
    lv_obj_clear_flag(head1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head2 = lv_obj_create(parent);
    lv_obj_set_size(head2, 34, 26);
    lv_obj_set_pos(head2, 128, 92);
    lv_obj_set_style_radius(head2, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(head2, col(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(head2, 235, 0);
    lv_obj_set_style_border_width(head2, 0, 0);
    lv_obj_clear_flag(head2, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *stem = lv_obj_create(parent);
        lv_obj_set_size(stem, 7, 74);
        lv_obj_set_pos(stem, i == 0 ? 101 : 155, i == 0 ? 62 : 46);
        lv_obj_set_style_bg_color(stem, col(0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(stem, 235, 0);
        lv_obj_set_style_border_width(stem, 0, 0);
        lv_obj_set_style_radius(stem, 3, 0);
        lv_obj_clear_flag(stem, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t *beam = lv_obj_create(parent);
    lv_obj_set_size(beam, 61, 12);
    lv_obj_set_pos(beam, 101, 46);
    lv_obj_set_style_bg_color(beam, col(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(beam, 235, 0);
    lv_obj_set_style_border_width(beam, 0, 0);
    lv_obj_set_style_radius(beam, 4, 0);
    lv_obj_clear_flag(beam, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *circle_btn(lv_obj_t *parent, int size, int x, int y, uint32_t bg,
                            lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, col(bg), 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    }
    return b;
}

/* ---- 网络电台 ---- */
typedef struct { const char *name; const char *url; } station_t;
static const station_t s_stations[] = {
    { "Radio Paradise 主频道", "http://stream.radioparadise.com/mp3-128" },
    { "Radio Paradise 舒缓",   "http://stream.radioparadise.com/mellow-128" },
    { "Radio Paradise 摇滚",   "http://stream.radioparadise.com/rock-128" },
    { "Radio Paradise 世界",   "http://stream.radioparadise.com/global-128" },
};
#define STATION_CNT ((int)(sizeof(s_stations)/sizeof(s_stations[0])))

static void station_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= STATION_CNT) {
        return;
    }
    strlcpy((char *)s_stream_url, s_stations[i].url, sizeof(s_stream_url));
    s_cmd = 1;
    s_playing = false;
    if (s_sem) xSemaphoreGive(s_sem);
    lv_label_set_text(s_lbl_title, s_stations[i].name);
    lv_label_set_text(s_lbl_sub, "网络电台（在线流）");
    s_paused = false;
    s_pos_ms = 0;
    s_total_ms = 0;
    s_lrc_cnt = 0;                  /* 电台没有歌词 */
    lv_obj_add_flag(s_radio, LV_OBJ_FLAG_HIDDEN);
    ui_shell_toast("正在连接电台…");
}

static void radio_close_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(s_radio, LV_OBJ_FLAG_HIDDEN);
}

static void radio_open_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_remove_flag(s_radio, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_radio);
}

/* 每次重建界面前清空控件指针。
 * App 没开"后台"时退出会销毁整个界面，静态指针就此悬空；
 * 下一次 create 里若还有函数先用到它（比如按旧按钮改样式），
 * 就会 Load access fault 直接重启。 */
static void reset_ui_refs(void)
{
    s_cover = NULL;
    s_lbl_title = NULL;
    s_lbl_sub = NULL;
    s_bar = NULL;
    s_lbl_cur = NULL;
    s_lbl_total = NULL;
    s_btn_play = NULL;
    s_lbl_play = NULL;
    s_playlist = NULL;
    s_plist = NULL;
    s_lbl_hint = NULL;
    s_lbl_empty = NULL;
    s_search = NULL;
    s_search_ta = NULL;
    s_search_list = NULL;
    s_vol_bar = NULL;
    s_btn_mute = NULL;
    s_radio = NULL;
    s_lbl_fav = NULL;
    s_lbl_dl = NULL;
    memset(s_lyric_lbl, 0, sizeof(s_lyric_lbl));
    s_lbl_src = NULL;
}

static void create(lv_obj_t *parent)
{
    reset_ui_refs();
    ui_app_topbar(parent, "音乐");   /* 现在是空实现，只为兼容 */
    lv_obj_set_style_bg_color(parent, col(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);

    rescan();
    fav_load();
    hist_load();
    if (s_index >= s_count) {
        s_index = 0;
    }

    /* 顶部工具行（坐标按有标题栏算，创建后整体上移 34px） */
    lv_obj_t *sb = lv_button_create(parent);
    lv_obj_set_size(sb, 96, 34);
    lv_obj_set_pos(sb, 12, 40);
    lv_obj_set_style_radius(sb, 17, 0);
    lv_obj_set_style_bg_color(sb, col(0x1A2230), 0);
    lv_obj_add_event_cb(sb, search_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sbl = ui_label(sb, "搜索", &ui_cjk_16, C_TXT);
    lv_obj_center(sbl);

    lv_obj_t *db = lv_button_create(parent);
    lv_obj_set_size(db, 96, 34);
    lv_obj_set_pos(db, 192, 40);
    lv_obj_set_style_radius(db, 17, 0);
    lv_obj_set_style_bg_color(db, col(0x1A2230), 0);
    lv_obj_add_event_cb(db, dl_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_dl = ui_label(db, "下载", &ui_cjk_16, C_TXT);
    lv_obj_center(s_lbl_dl);

    lv_obj_t *fb = lv_button_create(parent);
    lv_obj_set_size(fb, 96, 34);
    lv_obj_set_pos(fb, 372, 40);
    lv_obj_set_style_radius(fb, 17, 0);
    lv_obj_set_style_bg_color(fb, col(0x1A2230), 0);
    lv_obj_add_event_cb(fb, fav_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_fav = ui_label(fb, "收藏", &ui_cjk_16, C_TXT);
    lv_obj_center(s_lbl_fav);

    /* 歌词面板（原来这里是唱片圆圈） */
    s_cover = lv_obj_create(parent);
    lv_obj_set_size(s_cover, 440, 186);
    lv_obj_set_pos(s_cover, 20, 78);
    lv_obj_set_style_radius(s_cover, 14, 0);
    lv_obj_set_style_bg_color(s_cover, col(0x0E141C), 0);
    lv_obj_set_style_bg_opa(s_cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cover, 1, 0);
    lv_obj_set_style_border_color(s_cover, col(0x1B2230), 0);
    lv_obj_set_style_pad_all(s_cover, 0, 0);
    lv_obj_clear_flag(s_cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_cover, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 5; i++) {
        lv_obj_t *l = ui_label(s_cover, "", &ui_cjk_16, 0x5B6B85);
        lv_obj_set_width(l, 420);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(l, 10, 8 + i * 34);
        s_lyric_lbl[i] = l;
    }
    lv_label_set_text(s_lyric_lbl[2], "暂无歌词");

    s_lbl_title = ui_label(parent, "—", &ui_cjk_16, C_TXT);
    lv_obj_set_width(s_lbl_title, 456);
    lv_obj_set_style_text_align(s_lbl_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_title, 12, 272);

    s_lbl_sub = ui_label(parent, "本地音乐", &ui_cjk_16, 0x7B8CA6);
    lv_obj_set_width(s_lbl_sub, 456);
    lv_obj_set_style_text_align(s_lbl_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_lbl_sub, 12, 298);

    s_bar = lv_bar_create(parent);
    lv_obj_set_size(s_bar, 440, 6);
    lv_obj_set_pos(s_bar, 20, 322);
    lv_obj_set_style_bg_color(s_bar, col(0x25303F), LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, col(0xE8E8E8), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 3, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 1000);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_lbl_cur = ui_label(parent, "0:00", &lv_font_montserrat_14, 0x7B8CA6);
    lv_obj_set_pos(s_lbl_cur, 20, 330);
    s_lbl_total = ui_label(parent, "0:00", &lv_font_montserrat_14, 0x7B8CA6);
    lv_obj_set_pos(s_lbl_total, 442, 330);

    /* 控制按钮 */
    lv_obj_t *prev = circle_btn(parent, 56, 104, 380, 0x1A2230, prev_btn_cb);
    lv_obj_t *pl = ui_label(prev, "|<", &lv_font_montserrat_20, C_TXT);
    lv_obj_center(pl);

    s_btn_play = circle_btn(parent, 80, 200, 368, 0xE8E8E8, play_btn_cb);
    s_lbl_play = ui_label(s_btn_play, ">", &lv_font_montserrat_32, 0x101418);
    lv_obj_center(s_lbl_play);

    lv_obj_t *next = circle_btn(parent, 56, 320, 380, 0x1A2230, next_btn_cb);
    lv_obj_t *nl = ui_label(next, ">|", &lv_font_montserrat_20, C_TXT);
    lv_obj_center(nl);

    lv_obj_t *listb = circle_btn(parent, 44, 424, 384, 0x1A2230, pl_btn_cb);
    lv_obj_t *ll = ui_label(listb, "列表", &ui_cjk_16, C_TXT);
    lv_obj_center(ll);

    s_lbl_hint = ui_label(parent, "", &ui_cjk_16, 0x44598A);
    lv_obj_set_pos(s_lbl_hint, 20, 452);

    s_lbl_empty = ui_label(parent, "", &ui_cjk_16, 0x44598A);

    /* ---- 播放列表浮层（带返回按钮） ---- */
    s_playlist = lv_obj_create(parent);
    lv_obj_set_size(s_playlist, 480, 480);
    lv_obj_set_pos(s_playlist, 0, 0);
    lv_obj_set_style_bg_color(s_playlist, col(0x000000), 0);
    lv_obj_set_style_bg_opa(s_playlist, 218, 0);
    lv_obj_set_style_border_width(s_playlist, 0, 0);
    lv_obj_set_style_radius(s_playlist, 0, 0);
    lv_obj_set_style_pad_all(s_playlist, 0, 0);
    lv_obj_clear_flag(s_playlist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *pt = ui_label(s_playlist, "播放列表", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(pt, 20, 14);

    lv_obj_t *favf = lv_button_create(s_playlist);
    lv_obj_set_size(favf, 88, 32);
    lv_obj_set_pos(favf, 176, 12);
    lv_obj_set_style_radius(favf, 16, 0);
    lv_obj_set_style_bg_color(favf, col(0x1A2230), 0);
    lv_obj_add_event_cb(favf, fav_list_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fl = ui_label(favf, "只看收藏", &ui_cjk_16, C_TXT);
    lv_obj_center(fl);

    lv_obj_t *back = lv_button_create(s_playlist);
    lv_obj_set_size(back, 96, 32);
    lv_obj_set_pos(back, 368, 12);
    lv_obj_set_style_radius(back, 16, 0);
    lv_obj_set_style_bg_color(back, col(0x2E4C7A), 0);
    lv_obj_add_event_cb(back, pl_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(back, "返回", &ui_cjk_16, C_TXT);
    lv_obj_center(bl);

    lv_obj_t *plist = lv_obj_create(s_playlist);
    s_plist = plist;
    lv_obj_set_size(plist, 440, 400);
    lv_obj_set_pos(plist, 20, 58);
    lv_obj_set_style_bg_color(plist, col(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(plist, 235, 0);
    lv_obj_set_style_border_width(plist, 0, 0);
    lv_obj_set_style_radius(plist, 14, 0);
    lv_obj_set_style_pad_all(plist, 8, 0);
    lv_obj_set_flex_flow(plist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(plist, LV_DIR_VER);

    /* ---- 搜索浮层 ---- */
    s_search = lv_obj_create(parent);
    lv_obj_set_size(s_search, 480, 480);
    lv_obj_set_pos(s_search, 0, 0);
    lv_obj_set_style_bg_color(s_search, col(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(s_search, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_search, 0, 0);
    lv_obj_set_style_radius(s_search, 0, 0);
    lv_obj_set_style_pad_all(s_search, 0, 0);
    lv_obj_clear_flag(s_search, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_search, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *st = ui_label(s_search, "搜索", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(st, 16, 10);


    lv_obj_t *srcb = lv_button_create(s_search);
    lv_obj_set_size(srcb, 104, 32);
    lv_obj_set_pos(srcb, 112, 8);
    lv_obj_set_style_radius(srcb, 16, 0);
    lv_obj_set_style_bg_color(srcb, col(0x2A3A2E), 0);
    lv_obj_add_event_cb(srcb, src_btn_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_src = ui_label(srcb, "源 酷我", &ui_cjk_16, C_TXT);
    lv_obj_center(s_lbl_src);

    lv_obj_t *as = lv_button_create(s_search);
    lv_obj_set_size(as, 104, 32);
    lv_obj_set_pos(as, 224, 8);
    lv_obj_set_style_radius(as, 16, 0);
    lv_obj_set_style_bg_color(as, col(0x4A2E5A), 0);
    lv_obj_add_event_cb(as, song_search_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *asl = ui_label(as, "搜歌", &ui_cjk_16, C_TXT);
    lv_obj_center(asl);

    lv_obj_t *sc = lv_button_create(s_search);
    lv_obj_set_size(sc, 88, 32);
    lv_obj_set_pos(sc, 380, 8);
    lv_obj_set_style_radius(sc, 16, 0);
    lv_obj_set_style_bg_color(sc, col(0x2E4C7A), 0);
    lv_obj_add_event_cb(sc, search_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scl = ui_label(sc, "返回", &ui_cjk_16, C_TXT);
    lv_obj_center(scl);

    /* 关键字显示（自绘键盘输入） */
    s_search_ta = lv_label_create(s_search);
    lv_obj_set_size(s_search_ta, 440, 46);
    lv_obj_set_pos(s_search_ta, 20, 52);
    lv_obj_set_style_bg_color(s_search_ta, col(0x141A24), 0);
    lv_obj_set_style_bg_opa(s_search_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_search_ta, &ui_cjk_16, 0);
    lv_obj_set_style_text_color(s_search_ta, col(C_TXT), 0);
    lv_obj_set_style_pad_left(s_search_ta, 10, 0);
    lv_obj_set_style_radius(s_search_ta, 12, 0);
    lv_label_set_text(s_search_ta, "");

    s_search_list = lv_obj_create(s_search);
    lv_obj_set_size(s_search_list, 440, 152);
    lv_obj_set_pos(s_search_list, 20, 106);
    lv_obj_set_style_bg_color(s_search_list, col(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(s_search_list, 235, 0);
    lv_obj_set_style_border_width(s_search_list, 0, 0);
    lv_obj_set_style_radius(s_search_list, 12, 0);
    lv_obj_set_style_pad_all(s_search_list, 8, 0);
    lv_obj_set_flex_flow(s_search_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_search_list, LV_DIR_VER);

    /* 自绘键盘（稳定，不依赖 lv_keyboard） */
    ui_keypad_build_text(s_search, 260);
    ui_keypad_bind(s_search_kw, sizeof(s_search_kw), s_search_ta, search_mark_dirty, NULL);

    /* ---- 网络电台浮层 ---- */
    s_radio = lv_obj_create(parent);
    lv_obj_set_size(s_radio, 480, 480);
    lv_obj_set_pos(s_radio, 0, 0);
    lv_obj_set_style_bg_color(s_radio, col(0x0A0D12), 0);
    lv_obj_set_style_bg_opa(s_radio, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_radio, 0, 0);
    lv_obj_set_style_radius(s_radio, 0, 0);
    lv_obj_set_style_pad_all(s_radio, 0, 0);
    lv_obj_clear_flag(s_radio, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_radio, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *rt = ui_label(s_radio, "网络电台", &ui_cjk_16, C_TXT);
    lv_obj_set_pos(rt, 20, 14);

    lv_obj_t *rc = lv_button_create(s_radio);
    lv_obj_set_size(rc, 96, 34);
    lv_obj_set_pos(rc, 368, 12);
    lv_obj_set_style_radius(rc, 17, 0);
    lv_obj_set_style_bg_color(rc, col(0x2E4C7A), 0);
    lv_obj_add_event_cb(rc, radio_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rcl = ui_label(rc, "返回", &ui_cjk_16, C_TXT);
    lv_obj_center(rcl);

    for (int i = 0; i < STATION_CNT; i++) {
        lv_obj_t *row = lv_button_create(s_radio);
        lv_obj_set_size(row, 440, 56);
        lv_obj_set_pos(row, 20, 64 + i * 66);
        lv_obj_set_style_bg_color(row, col(0x141A24), 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_add_event_cb(row, station_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *nm = ui_label(row, s_stations[i].name, &ui_cjk_16, C_TXT);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 8, 0);
    }

    lv_obj_t *rnote = ui_label(s_radio, "在线电台需要联网，走 MP3 软解播放", &ui_cjk_16, 0x5B6B85);
    lv_obj_set_pos(rnote, 20, 440);

    {
        nvs_handle_t h;
        int32_t v = 5;
        if (nvs_open("sys", NVS_READONLY, &h) == ESP_OK) {
            nvs_get_i32(h, "vol", &v);
            nvs_close(h);
        }
        vol_apply((int)v);
        s_vol_before_mute = s_vol > 0 ? s_vol : 5;
    }

    if (!s_sem) {
        s_sem = xSemaphoreCreateBinary();
        xTaskCreateWithCaps(player_task, "music", 32768, NULL, 3, NULL, MALLOC_CAP_SPIRAM);
    }
    update_track_ui();
}

static void tick(void)
{
    /* 网络搜索结果到达 / 键盘输入 → 刷新搜索列表（节流） */
    static int last_net = -1;
    bool vis = s_search && !lv_obj_has_flag(s_search, LV_OBJ_FLAG_HIDDEN);
    if (s_net_cnt != last_net) {
        last_net = s_net_cnt;
        s_search_dirty = true;
    }
    if (s_search_dirty && vis) {
        s_search_dirty = false;
        search_rebuild();
    }
    /* 播放状态 → UI */
    if (s_lbl_play) {
        lv_label_set_text(s_lbl_play, (s_playing && !s_paused) ? "II" : ">");
    }

    /* 收藏按钮跟随当前曲目 */
    static char fav_of[NAME_LEN];
    if (strcmp(fav_of, s_cur_name) != 0) {
        strlcpy(fav_of, s_cur_name, sizeof(fav_of));
        if (s_lbl_fav && s_cur_name[0]) {
            bool on = is_fav(s_cur_name);
            lv_label_set_text(s_lbl_fav, on ? "已收藏" : "收藏");
            lv_obj_t *b = lv_obj_get_parent(s_lbl_fav);
            if (b) {
                lv_obj_set_style_bg_color(b, col(on ? 0xB0304A : 0x1A2230), 0);
            }
        }
    }

    /* 下载：状态文字 + 完成后刷新曲库 */
    if (s_lbl_dl) {
        lv_label_set_text(s_lbl_dl, s_dl_busy ? "下载中" : "下载");
    }
    if (s_rescan_req) {
        s_rescan_req = false;
        rescan();                   /* 刚下载完：新歌进本地列表 */
        playlist_rebuild();
    }
    if (s_dl_result) {
        int r = s_dl_result;
        s_dl_result = 0;
        ui_shell_toast(s_dl_msg);
        (void)r;
    }

    /* 歌词跟随播放进度滚动 */
    lyric_show(s_pos_ms);
    if (s_playing || s_total_ms > 0) {
        int p = (s_total_ms > 0) ? (int)((int64_t)s_pos_ms * 1000 / s_total_ms) : 0;
        if (p > 1000) p = 1000;
        lv_bar_set_value(s_bar, p, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_lbl_cur, "%d:%02d", s_pos_ms / 60000, (s_pos_ms / 1000) % 60);
        lv_label_set_text_fmt(s_lbl_total, "%d:%02d", s_total_ms / 60000, (s_total_ms / 1000) % 60);
    }
    if (s_dl_busy) {
        lv_label_set_text(s_lbl_hint, "正在下载到 TF 卡…");
    } else if (s_err == 2) {
        lv_label_set_text(s_lbl_hint, "音频格式不支持");
    } else if (s_paused) {
        lv_label_set_text(s_lbl_hint, "已暂停");
    } else if (!s_playing && (s_pos_ms > 0 || s_total_ms > 0)) {
        lv_label_set_text(s_lbl_hint, "已暂停");
    } else {
        lv_label_set_text(s_lbl_hint, "");
    }
}

static void on_show(void)
{
    ui_shell_set_tick_cb(tick);
    if (s_count == 0) {
        rescan();
        update_track_ui();
    }
}

static void on_hide(void)
{
    ui_shell_set_tick_cb(NULL);
    /* 后台被关掉时退出即停播（界面马上会被销毁） */
    if (ui_shell_app_shutting()) {
        s_cmd = 3;
        s_paused = false;
        s_playing = false;
        if (s_sem) {
            xSemaphoreGive(s_sem);
        }
        ESP_LOGI(TAG, "closed: stop playback");
    }
}

static void on_hide_unused(void)
{
    ui_shell_set_tick_cb(NULL);
}

const ui_app_t ui_app_music = {
    .name = "音乐",
    .symbol = LV_SYMBOL_AUDIO,
    .color = 0x8C2F39,
    .create = create,
    .on_show = on_show,
    .on_hide = on_hide,
};

/* 自测钩子（调试用，不影响正常使用） */
void ui_music_selftest(int mode)
{
    lvgl_port_lock(0);
    ESP_LOGI(TAG, "selftest mode=%d count=%d playing=%d", mode, s_count, (int)s_playing);
    if (mode == 1 && s_search) {
        lv_obj_remove_flag(s_search, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_search);
        s_search_kw[0] = 0;
        ui_keypad_bind(s_search_kw, sizeof(s_search_kw), s_search_ta, search_mark_dirty, NULL);
        search_rebuild();
        ESP_LOGI(TAG, "selftest: search opened");
    } else if (mode == 2) {
        strlcpy((char *)s_stream_url, s_stations[0].url, sizeof(s_stream_url));
        s_cmd = 1;
        s_playing = false;
        if (s_sem) xSemaphoreGive(s_sem);
        ESP_LOGI(TAG, "selftest: radio play requested");
    } else if (mode == 3) {
        vol_apply(85);
        ESP_LOGI(TAG, "selftest: volume 85");
    } else if (mode == 4) {
        if (s_search) lv_obj_add_flag(s_search, LV_OBJ_FLAG_HIDDEN);
    } else if (mode == 5) {
        strlcpy(s_search_kw, "jazz", sizeof(s_search_kw));
        strlcpy(s_net_kw, s_search_kw, sizeof(s_net_kw));
        s_net_cnt = 0;
        s_net_busy = true;
        BaseType_t ok2 = xTaskCreateWithCaps(net_search_task, "net_srch", 12288, (void *)0, 3, NULL, MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "net task create=%d", (int)ok2);
    } else if (mode == 6) {
        strlcpy(s_search_kw, "jazz", sizeof(s_search_kw));
        strlcpy(s_net_kw, s_search_kw, sizeof(s_net_kw));
        s_net_cnt = 0;
        s_net_busy = true;
        BaseType_t ok3 = xTaskCreateWithCaps(net_search_task, "net_srch", 12288, (void *)1, 3, NULL, MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "audius task create=%d", (int)ok3);
    } else if (mode == 8) {
        /* 国内曲库搜歌（酷我） */
        strlcpy(s_search_kw, "天外来物", sizeof(s_search_kw));
        strlcpy(s_net_kw, s_search_kw, sizeof(s_net_kw));
        s_net_cnt = 0;
        s_net_busy = true;
        BaseType_t ok4 = xTaskCreateWithCaps(net_search_task, "net_srch", 12288, (void *)2, 3, NULL, MALLOC_CAP_SPIRAM);
        ESP_LOGI(TAG, "kuwo task create=%d", (int)ok4);
    } else if (mode == 11) {
        s_paused = true;
        ESP_LOGI(TAG, "selftest: paused");
    } else if (mode == 12) {
        s_paused = false;
        ESP_LOGI(TAG, "selftest: resumed");
    } else if (mode == 13) {
        ESP_LOGI(TAG, "progress pos=%d total=%d playing=%d paused=%d lrc=%d",
                 s_pos_ms, s_total_ms, (int)s_playing, (int)s_paused, s_lrc_cnt);
        for (int i = 0; i < s_lrc_cnt && i < 3; i++) {
            ESP_LOGI(TAG, "  lrc[%d] %d %s", i, s_lrc_ms[i], s_lrc[i]);
        }
    } else if (mode == 10) {
        char out[256] = "";
        bool okr = false;
        if (s_net_cnt > 0 && strncmp(s_net_urls[0], "kuwo://", 7) == 0) {
            okr = kuwo_resolve(s_net_urls[0] + 7, out, sizeof(out));
        }
        ESP_LOGI(TAG, "selftest resolve=%d url=%.100s", (int)okr, okr ? out : "");
    } else if (mode == 9) {
        ESP_LOGI(TAG, "selftest: net_cnt=%d", s_net_cnt);
        for (int i = 0; i < s_net_cnt; i++) {
            ESP_LOGI(TAG, "  [%d] %s | %s", i, s_net_names[i], s_net_urls[i]);
        }
    } else if (mode == 7) {
        if (s_net_cnt > 0) {
            strlcpy((char *)s_stream_url, s_net_urls[0], sizeof(s_stream_url));
            s_cmd = 1;
            s_playing = false;
            s_paused = false;
            strlcpy(s_cur_name, s_net_names[0], sizeof(s_cur_name));
            strlcpy(s_cur_url, s_net_urls[0], sizeof(s_cur_url));
            if (s_sem) xSemaphoreGive(s_sem);
            lyric_request(s_net_names[0], "");
            ESP_LOGI(TAG, "play net[0]=%s sem=%p", s_net_urls[0], s_sem);
        }
    }
    lvgl_port_unlock();
}
