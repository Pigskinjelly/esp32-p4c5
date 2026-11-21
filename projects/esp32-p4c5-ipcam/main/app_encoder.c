/*
 * H.264 硬件编码器实现（V4L2 M2M，/dev/video11）
 */
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "linux/videodev2.h"

#include "esp_video_device.h"
#include "app_encoder.h"

/* ESP-IDF 的 newlib 未定义 MAP_FAILED */
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

static const char *TAG = "encoder";

typedef struct {
    int fd;
    int width;
    int height;
    int fps;

    uint8_t *cap_buf;       /* CAPTURE(码流) mmap 缓冲 */
    size_t   cap_len;

    const uint8_t *sps;
    size_t   sps_len;
    const uint8_t *pps;
    size_t   pps_len;
    uint8_t  sps_store[APP_ENC_MAX_SPS_LEN];
    uint8_t  pps_store[APP_ENC_MAX_PPS_LEN];

    app_enc_out_cb_t cb;
    void *ctx;

    uint32_t frames;
    uint32_t keyframes;
    uint32_t last_size;
    bool     streaming;
} encoder_t;

static encoder_t s_enc = { .fd = -1 };

/* 从 Annex-B 码流中提取 SPS(7)/PPS(8) 裸 NAL，去掉起始码 */
static void encoder_parse_sps_pps(const uint8_t *buf, size_t len)
{
    size_t i = 0;

    while (i + 4 < len) {
        /* 查找起始码 00 00 01 或 00 00 00 01 */
        if (buf[i] != 0x00 || buf[i + 1] != 0x00) {
            i++;
            continue;
        }

        size_t sc = 0;
        if (buf[i + 2] == 0x01) {
            sc = 3;
        } else if (buf[i + 2] == 0x00 && buf[i + 3] == 0x01) {
            sc = 4;
        } else {
            i++;
            continue;
        }

        size_t nal_start = i + sc;
        if (nal_start >= len) {
            break;
        }

        uint8_t nal_type = buf[nal_start] & 0x1F;

        /* 找下一个起始码作为本 NAL 结尾 */
        size_t j = nal_start + 1;
        while (j + 3 < len) {
            if (buf[j] == 0x00 && buf[j + 1] == 0x00 &&
                (buf[j + 2] == 0x01 || (buf[j + 2] == 0x00 && buf[j + 3] == 0x01))) {
                break;
            }
            j++;
        }
        size_t nal_len = j - nal_start;

        if (nal_type == 7 && nal_len > 0 && nal_len <= APP_ENC_MAX_SPS_LEN) {
            memcpy(s_enc.sps_store, &buf[nal_start], nal_len);
            s_enc.sps = s_enc.sps_store;
            s_enc.sps_len = nal_len;
        } else if (nal_type == 8 && nal_len > 0 && nal_len <= APP_ENC_MAX_PPS_LEN) {
            memcpy(s_enc.pps_store, &buf[nal_start], nal_len);
            s_enc.pps = s_enc.pps_store;
            s_enc.pps_len = nal_len;
        }

        i = j;
    }
}

static esp_err_t encoder_set_ctrl(uint32_t id, int32_t value)
{
    struct v4l2_ext_control ctrl = { .id = id, .value = value };
    struct v4l2_ext_controls ctrls = {
        .ctrl_class = V4L2_CID_CODEC_CLASS,
        .count = 1,
        .controls = &ctrl,
    };

    if (ioctl(s_enc.fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
        ESP_LOGW(TAG, "set ctrl 0x%08x=%d failed: %d", id, (int)value, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t app_encoder_init(int width, int height, int fps, int bitrate,
                           app_enc_out_cb_t cb, void *ctx)
{
    if (s_enc.fd >= 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (width <= 0 || height <= 0 || (width & 0xF) || (height & 0xF)) {
        ESP_LOGE(TAG, "invalid size %dx%d (must be 16-aligned)", width, height);
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_enc, 0, sizeof(s_enc));
    s_enc.fd = -1;              /* memset 会置 0，而 0 是有效 fd，必须显式复位 */
    s_enc.width = width;
    s_enc.height = height;
    s_enc.fps = fps;
    s_enc.cb = cb;
    s_enc.ctx = ctx;

    s_enc.fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDWR);
    if (s_enc.fd < 0) {
        ESP_LOGE(TAG, "open %s failed: %d", ESP_VIDEO_H264_DEVICE_NAME, errno);
        return ESP_FAIL;
    }

    /* 1. 配置 OUTPUT 流（原始 YUV420） */
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix.bytesperline = width;
    fmt.fmt.pix.sizeimage = width * height * 3 / 2;
    if (ioctl(s_enc.fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "S_FMT OUTPUT failed: %d", errno);
        goto fail;
    }

    struct v4l2_requestbuffers req = {
        .count = 1,
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    if (ioctl(s_enc.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS OUTPUT failed: %d", errno);
        goto fail;
    }

    /* 2. 配置 CAPTURE 流（H.264 码流） */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    if (ioctl(s_enc.fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "S_FMT CAPTURE failed: %d", errno);
        goto fail;
    }

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_enc.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS CAPTURE failed: %d", errno);
        goto fail;
    }

    struct v4l2_buffer buf = { 0 };
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (ioctl(s_enc.fd, VIDIOC_QUERYBUF, &buf) != 0) {
        ESP_LOGE(TAG, "QUERYBUF CAPTURE failed: %d", errno);
        goto fail;
    }

    s_enc.cap_len = buf.length;
    s_enc.cap_buf = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_enc.fd, buf.m.offset);
    if (s_enc.cap_buf == MAP_FAILED) {
        s_enc.cap_buf = NULL;
        ESP_LOGE(TAG, "mmap CAPTURE failed: %d", errno);
        goto fail;
    }

    if (ioctl(s_enc.fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "QBUF CAPTURE failed: %d", errno);
        goto fail;
    }

    /* 3. 码控参数 */
    if (bitrate < 25000) {
        bitrate = 25000;
    } else if (bitrate > 2500000) {
        bitrate = 2500000;
    }
    encoder_set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, bitrate);
    encoder_set_ctrl(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, fps);   /* 每秒一个 I 帧 */
    encoder_set_ctrl(V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 22);
    encoder_set_ctrl(V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 42);

    /* 4. 开流 */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_enc.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON CAPTURE failed: %d", errno);
        goto fail;
    }
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(s_enc.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON OUTPUT failed: %d", errno);
        goto fail;
    }

    s_enc.streaming = true;
    ESP_LOGI(TAG, "H.264 encoder ready: %dx%d @%dfps, %d bps", width, height, fps, bitrate);
    return ESP_OK;

fail:
    if (s_enc.cap_buf) {
        munmap(s_enc.cap_buf, s_enc.cap_len);
        s_enc.cap_buf = NULL;
    }
    if (s_enc.fd >= 0) {
        close(s_enc.fd);
        s_enc.fd = -1;
    }
    return ESP_FAIL;
}

esp_err_t app_encoder_encode(const uint8_t *yuv420, size_t len, uint64_t pts_us)
{
    if (s_enc.fd < 0 || !s_enc.streaming) {
        return ESP_ERR_INVALID_STATE;
    }
    if (yuv420 == NULL || len < (size_t)(s_enc.width * s_enc.height * 3 / 2)) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 1. 原始帧以 USERPTR 直接喂入，零拷贝 */
    struct v4l2_buffer out = { 0 };
    out.index = 0;
    out.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    out.memory = V4L2_MEMORY_USERPTR;
    out.m.userptr = (unsigned long)yuv420;
    out.length = len;
    if (ioctl(s_enc.fd, VIDIOC_QBUF, &out) != 0) {
        ESP_LOGW(TAG, "QBUF OUTPUT failed: %d", errno);
        return ESP_FAIL;
    }

    /* 2. 取码流 */
    struct v4l2_buffer cap = { 0 };
    cap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cap.memory = V4L2_MEMORY_MMAP;
    if (ioctl(s_enc.fd, VIDIOC_DQBUF, &cap) != 0) {
        ESP_LOGW(TAG, "DQBUF CAPTURE failed: %d", errno);
        return ESP_FAIL;
    }

    size_t nal_len = cap.bytesused;
    bool key = false;

    if (nal_len > 0 && nal_len <= s_enc.cap_len) {
        /* 判断是否含 IDR(5) 或 SPS(7) */
        for (size_t i = 0; i + 3 < nal_len; i++) {
            if (s_enc.cap_buf[i] == 0x00 && s_enc.cap_buf[i + 1] == 0x00 &&
                (s_enc.cap_buf[i + 2] == 0x01 ||
                 (s_enc.cap_buf[i + 2] == 0x00 && s_enc.cap_buf[i + 3] == 0x01))) {
                size_t p = (s_enc.cap_buf[i + 2] == 0x01) ? i + 3 : i + 4;
                if (p < nal_len) {
                    uint8_t t = s_enc.cap_buf[p] & 0x1F;
                    if (t == 5 || t == 7) {
                        key = true;
                    }
                }
                break;
            }
        }

        encoder_parse_sps_pps(s_enc.cap_buf, nal_len);

        s_enc.frames++;
        if (key) {
            s_enc.keyframes++;
        }
        s_enc.last_size = nal_len;

        if (s_enc.cb) {
            app_enc_frame_t f = {
                .data = s_enc.cap_buf,
                .len = nal_len,
                .keyframe = key,
                .pts_us = pts_us,
            };
            s_enc.cb(&f, s_enc.ctx);
        }
    } else {
        ESP_LOGW(TAG, "bad nal length %u (cap %u)", (unsigned)nal_len, (unsigned)s_enc.cap_len);
    }

    /* 3. 归还缓冲 */
    ioctl(s_enc.fd, VIDIOC_QBUF, &cap);
    ioctl(s_enc.fd, VIDIOC_DQBUF, &out);

    return ESP_OK;
}

esp_err_t app_encoder_set_bitrate(int bitrate)
{
    if (s_enc.fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bitrate < 25000) {
        bitrate = 25000;
    } else if (bitrate > 2500000) {
        bitrate = 2500000;
    }
    return encoder_set_ctrl(V4L2_CID_MPEG_VIDEO_BITRATE, bitrate);
}

bool app_encoder_get_sps(const uint8_t **data, size_t *len)
{
    if (!s_enc.sps || s_enc.sps_len == 0) {
        return false;
    }
    *data = s_enc.sps;
    *len = s_enc.sps_len;
    return true;
}

bool app_encoder_get_pps(const uint8_t **data, size_t *len)
{
    if (!s_enc.pps || s_enc.pps_len == 0) {
        return false;
    }
    *data = s_enc.pps;
    *len = s_enc.pps_len;
    return true;
}

void app_encoder_get_stats(uint32_t *frames, uint32_t *keyframes, uint32_t *last_size)
{
    if (frames) {
        *frames = s_enc.frames;
    }
    if (keyframes) {
        *keyframes = s_enc.keyframes;
    }
    if (last_size) {
        *last_size = s_enc.last_size;
    }
}

void app_encoder_deinit(void)
{
    if (s_enc.fd < 0) {
        return;
    }

    if (s_enc.streaming) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        ioctl(s_enc.fd, VIDIOC_STREAMOFF, &type);
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_enc.fd, VIDIOC_STREAMOFF, &type);
        s_enc.streaming = false;
    }

    if (s_enc.cap_buf) {
        munmap(s_enc.cap_buf, s_enc.cap_len);
        s_enc.cap_buf = NULL;
    }

    close(s_enc.fd);
    s_enc.fd = -1;
    ESP_LOGI(TAG, "encoder closed");
}
