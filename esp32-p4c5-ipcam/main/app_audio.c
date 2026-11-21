/*
 * ESP32-P4C5 整机 Demo — 音频实现（ES8311 编解码 + NS4150B 功放 + 麦克风回环）
 *
 * 基于 esp_codec_dev 1.6.x（内置 ES8311 驱动，CONFIG_CODEC_ES8311_SUPPORT=y）。
 * 硬件连接（V1.5 底板）：
 *   ES8311 I2C 地址 0x18（7bit，CE 经 10K 下拉），与相机共用 I2C（GPIO7=SDA, GPIO8=SCL）
 *   I2S0：GPIO9=DOUT(→codec DSDIN)、GPIO11=DIN(←codec ASDOUT)、GPIO10=WS、GPIO12=BCLK、GPIO13=MCLK
 *   功放 NS4150B STD：GPIO53，高电平使能（本组件直接控制，不走 codec 的 pa_pin）
 *   麦克风：板上 LMA3729 模拟麦，接入 ES8311 MIC1P/MIC1N
 */
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_common.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "nvs.h"

static const char *TAG = "app_audio";

/* I2S0 引脚（对应 V1.5 底板 ES8311 连接） */
#define I2S_BCLK_IO         (GPIO_NUM_12)
#define I2S_WS_IO           (GPIO_NUM_10)
#define I2S_DOUT_IO         (GPIO_NUM_9)   /* P4 -> ES8311 DSDIN */
#define I2S_DIN_IO          (GPIO_NUM_11)  /* ES8311 ASDOUT -> P4 */
#define I2S_MCLK_IO         (GPIO_NUM_13)

/* NS4150B 功放使能（STD 高有效） */
#define PA_CTRL_IO          (GPIO_NUM_53)

#define SAMPLE_RATE         (16000)

static esp_codec_dev_handle_t s_codec_dev;
static int s_volume = 5;   /* 默认很小音量，避免开机爆音 */

esp_err_t app_audio_init(i2c_master_bus_handle_t i2c_bus)
{
    /* 1. 功放使能（NS4150B STD 拉高；codec 侧 pa_pin=-1 不干预） */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << PA_CTRL_IO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&pa_cfg));
    gpio_set_level(PA_CTRL_IO, 1);
    ESP_LOGI(TAG, "PA enabled (GPIO%d)", PA_CTRL_IO);

    /* 2. I2S 通道（P4 为主，输出 MCLK 给 ES8311） */
    i2s_chan_handle_t i2s_tx = NULL;
    i2s_chan_handle_t i2s_rx = NULL;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;      /* 缓冲深一些，避免欠载爆音 */
    chan_cfg.dma_frame_num = 256;
    chan_cfg.auto_clear = true;     /* DMA 空了自动补 0，而不是重放旧数据（防"滋啦"） */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, &i2s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_MCLK_IO,
            .bclk = I2S_BCLK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_DOUT_IO,
            .din = I2S_DIN_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx, &std_cfg));

    /* 3. codec 控制接口（I2C，复用共享总线；addr 为 8 位地址 0x30） */
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,  /* 0x30 = 0x18 << 1 */
        .bus_handle = i2c_bus,
        .clock_speed_hz = 100000,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "failed to create codec i2c ctrl interface");
        return ESP_FAIL;
    }

    /* 4. ES8311 codec 接口 */
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = NULL,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,   /* 回环：ADC + DAC 全双工 */
        .pa_pin = -1,          /* 功放由本组件直接控制 */
        .pa_reverted = false,
        .master_mode = false,  /* P4 是 I2S 主，ES8311 为从 */
        .use_mclk = true,
        .digital_mic = false,  /* 板上为模拟麦克风 LMA3729 */
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
            .pa_gain = 0,
        },
        .no_dac_ref = false,
        .mclk_div = 0,         /* 默认 256 分频 */
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "failed to create ES8311 codec interface");
        return ESP_FAIL;
    }

    /* 5. codec 数据接口（I2S） */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = i2s_rx,
        .tx_handle = i2s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "failed to create codec i2s data interface");
        return ESP_FAIL;
    }

    /* 6. 创建 codec 设备 */
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,  /* 必须显式指定，否则为 0(NONE)，esp_codec_dev_new 返回 NULL */
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_codec_dev == NULL) {
        ESP_LOGE(TAG, "failed to create codec device");
        return ESP_FAIL;
    }

    /* 7. 打开 codec */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = SAMPLE_RATE,
        .mclk_multiple = 0,   /* 256 */
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_codec_dev, &fs));
    {
        int32_t vol = 5;
        int32_t ver = 0;
        nvs_handle_t h;
        if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
            nvs_get_i32(h, "vol", &vol);
            nvs_get_i32(h, "vol_ver", &ver);
            /* 一次性迁移：老固件默认 60 太吵、后来又被设成 0 没声音；
             * 统一拉回 5（之后用户自己调，不再覆盖） */
            if (ver < 3) {
                vol = 5;
                nvs_set_i32(h, "vol", vol);
                nvs_set_i32(h, "vol_ver", 3);
                nvs_commit(h);
            }
            nvs_close(h);
        }
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        s_volume = (int)vol;
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_codec_dev, (int)vol));
        ESP_LOGI(TAG, "volume = %d", (int)vol);
    }
    ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(s_codec_dev, 30.0)); /* dB */

    ESP_LOGI(TAG, "ES8311 codec initialized (16kHz mono 16bit)");
    return ESP_OK;
}

void app_audio_loopback_task(void *arg)
{
    /* 约 20ms 的 16bit 单声道数据 */
    const size_t frame_bytes = SAMPLE_RATE * 2 * 20 / 1000; /* 640B */
    uint8_t *buf = calloc(1, frame_bytes);
    if (buf == NULL) {
        ESP_LOGE(TAG, "failed to alloc loopback buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "audio loopback task started");
    while (1) {
        int r = esp_codec_dev_read(s_codec_dev, buf, frame_bytes);
        if (r > 0) {
            esp_codec_dev_write(s_codec_dev, buf, r);
        }
    }
}

/* ================= 播放接口（音乐 App） =================
 *
 * 为什么要有环形缓冲 + 独立喂数任务：
 *   原来解码任务"解一帧 → 写一帧"，中间还要去 HTTP 读数据（可能阻塞几百毫秒）。
 *   I2S 的 DMA 在这段时间里跑空，ES8311 还在按 MCLK 出数 → 空转时把残留采样
 *   反复推给 DAC，听感就是"滋啦滋啦/电音"。本地文件读得快所以不明显，
 *   网络歌必然爆。
 *
 *   现在：解码任务只往环形缓冲里塞 PCM；一个高优先级任务专门按 20ms 一段
 *   喂 I2S，缓冲空了就补静音（干净无声，不是噪声），网络抖动被缓冲吸收。
 */

#define SINK_RING_BYTES   (64 * 1024)          /* 44.1k 立体声约 370ms */
#define SINK_RING_MASK    (SINK_RING_BYTES - 1)

static uint8_t *s_ring;
static volatile uint32_t s_ring_wr, s_ring_rd;
static TaskHandle_t s_sink_task;
static volatile bool s_sink_run;
static volatile bool s_flush_req;

/* 把数据写满（esp_codec_dev_write 可能只写一部分） */
static void codec_write_all(const uint8_t *p, size_t len)
{
    size_t done = 0;
    while (done < len) {
        int r = esp_codec_dev_write(s_codec_dev, (void *)(p + done), (int)(len - done));
        if (r <= 0) {
            break;
        }
        done += (size_t)r;
    }
}

static void audio_sink_task(void *arg)
{
    (void)arg;
    const size_t chunk = 4096;                  /* 44.1k 立体声约 23ms */
    uint8_t *silence = heap_caps_malloc(chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!silence) {
        s_sink_run = false;
        s_sink_task = NULL;
        vTaskDeleteWithCaps(NULL);   /* R-18：栈在 PSRAM */
        return;
    }
    memset(silence, 0, chunk);
    ESP_LOGI(TAG, "audio sink running");

    while (s_sink_run) {
        if (s_flush_req) {
            /* 由消费者清缓冲，避免生产/消费同时改 rd 导致下标错乱 */
            s_flush_req = false;
            s_ring_rd = s_ring_wr;
            continue;
        }
        uint32_t avail = s_ring_wr - s_ring_rd;
        if (avail == 0) {
            /* 缓冲空：补静音把 DMA 喂住，避免 I2S 空转出噪声 */
            codec_write_all(silence, chunk);
            continue;
        }
        if (avail > chunk) {
            avail = chunk;
        }
        uint32_t off = s_ring_rd & SINK_RING_MASK;
        uint32_t n = SINK_RING_BYTES - off;
        if (n > avail) {
            n = avail;
        }
        n &= ~3u;                               /* 按 16bit 立体声对齐 */
        if (n == 0) {
            continue;
        }
        codec_write_all(s_ring + off, n);
        __sync_synchronize();
        s_ring_rd += n;
    }

    heap_caps_free(silence);
    s_sink_task = NULL;
    vTaskDeleteWithCaps(NULL);   /* R-18：栈在 PSRAM */
}

static void sink_stop(void)
{
    if (!s_sink_task) {
        return;
    }
    s_sink_run = false;
    for (int i = 0; i < 100 && s_sink_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_sink_task = NULL;
}

static esp_err_t sink_start(void)
{
    if (!s_ring) {
        s_ring = heap_caps_malloc(SINK_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ring) {
            ESP_LOGE(TAG, "no memory for audio ring");
            return ESP_ERR_NO_MEM;
        }
    }
    s_ring_wr = s_ring_rd = 0;
    s_sink_run = true;
    /* 优先级高于解码任务和 LVGL：随时能补数据 */
    if (xTaskCreateWithCaps(audio_sink_task, "audio_out", 4096, NULL, 12, &s_sink_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_sink_run = false;
        s_sink_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t app_audio_out_format(int sample_rate, int channels)
{
    static int s_rate, s_ch;

    if (!s_codec_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate <= 0) {
        sample_rate = 44100;
    }
    if (channels <= 0 || channels > 2) {
        channels = 2;
    }
    /* 格式没变就别重开：close/open 会断一下 DMA，听起来像"咔"一声 */
    if (sample_rate == s_rate && channels == s_ch && s_sink_task) {
        return ESP_OK;
    }

    /* 先停喂数任务，再重开 codec（否则会往正在重配的 I2S 里写） */
    sink_stop();

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = (uint8_t)channels,
        .channel_mask = 0,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    esp_codec_dev_close(s_codec_dev);
    esp_err_t err = esp_codec_dev_open(s_codec_dev, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reopen codec %dHz/%dch failed: %s", sample_rate, channels, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "out format %dHz %dch", sample_rate, channels);

    s_rate = sample_rate;
    s_ch = channels;
    sink_start();
    return err;
}

int app_audio_out_write(const void *buf, size_t len)
{
    if (!s_codec_dev) {
        return -1;
    }
    if (!s_sink_task) {
        /* 没走格式设置就直接写：退化成直写，保证还能出声 */
        codec_write_all((const uint8_t *)buf, len);
        return (int)len;
    }
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = len;
    while (left > 0) {
        uint32_t used = s_ring_wr - s_ring_rd;
        uint32_t space = (used >= SINK_RING_BYTES) ? 0 : (SINK_RING_BYTES - used);
        if (space == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));       /* 消费者在追，等一会 */
            continue;
        }
        uint32_t off = s_ring_wr & SINK_RING_MASK;
        uint32_t n = SINK_RING_BYTES - off;
        if (n > space) {
            n = space;
        }
        if (n > left) {
            n = (uint32_t)left;
        }
        memcpy(s_ring + off, p, n);
        __sync_synchronize();
        s_ring_wr += n;
        p += n;
        left -= n;
    }
    return (int)len;
}

void app_audio_out_flush(void)
{
    s_flush_req = true;
}

void app_audio_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    if (s_codec_dev) {
        esp_codec_dev_set_out_vol(s_codec_dev, vol);
    }
    nvs_handle_t h;
    if (nvs_open("sys", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "vol", vol);
        nvs_commit(h);
        nvs_close(h);
    }
}

int app_audio_get_volume(void)
{
    return s_volume;
}

bool app_audio_ready(void)
{
    return s_codec_dev != NULL;
}
