/*
 * JPEG 拍照实现 —— /dev/video10 V4L2 M2M（硬件 JPEG 编码器）
 *
 * 用法严格对齐 esp_video 官方 uvc 例程：
 *   OUTPUT  : V4L2_MEMORY_USERPTR —— 直接指向调用者的 RGB565 缓冲（零拷贝）
 *   CAPTURE : V4L2_MEMORY_MMAP   —— 硬件写入 JPEG 码流
 *
 * 关键点（之前版本拍照失败的原因）：
 *   1. 每次编码后必须 VIDIOC_DQBUF 回收 OUTPUT 缓冲，否则下一帧 QBUF 拿不到空闲缓冲；
 *   2. JPEG 缓冲要等调用者用完再归还（QBUF），不能刚出队就归还；
 *   3. 质量用 VIDIOC_S_EXT_CTRLS + V4L2_CID_JPEG_CLASS 设置。
 */
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"

#include "esp_video_device.h"
#include "app_jpeg.h"

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

#define TAG "jpeg"

/* 画质：85~95。95 细节最好，体积约为 80 的 1.6 倍 */
#define JPEG_QUALITY        95

typedef struct {
    int fd;
    int width, height;
    size_t src_bytes;       /* 一帧 RGB565 源字节数 */

    uint8_t *cap_buf;       /* JPEG 码流 mmap 缓冲 */
    size_t   cap_len;
    bool     cap_queued;    /* 码流缓冲是否已归还驱动 */

    bool     streaming;
} jpeg_enc_t;

static jpeg_enc_t s_jp = { .fd = -1 };

static int xioctl(int fd, int req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static esp_err_t set_quality(int fd, int quality)
{
    struct v4l2_ext_controls ctrls = { 0 };
    struct v4l2_ext_control ctrl = { 0 };

    ctrls.ctrl_class = V4L2_CID_JPEG_CLASS;
    ctrls.count = 1;
    ctrls.controls = &ctrl;
    ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    ctrl.value = quality;
    if (xioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) {
        return ESP_OK;
    }
    /* 回落到 S_CTRL */
    struct v4l2_control c = { .id = V4L2_CID_JPEG_COMPRESSION_QUALITY, .value = quality };
    if (xioctl(fd, VIDIOC_S_CTRL, &c) == 0) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

esp_err_t app_jpeg_init(int width, int height)
{
    if (s_jp.fd >= 0) {
        if (s_jp.width == width && s_jp.height == height) {
            return ESP_OK;
        }
        app_jpeg_deinit();
    }
    memset(&s_jp, 0, sizeof(s_jp));
    s_jp.fd = -1;

    int fd = open(ESP_VIDEO_JPEG_DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s failed: errno %d", ESP_VIDEO_JPEG_DEVICE_NAME, errno);
        return ESP_FAIL;
    }

    /* 1. 输入格式（RGB565） */
    struct v4l2_format fmt = { 0 };
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "S_FMT output failed: errno %d", errno);
        goto fail;
    }
    s_jp.src_bytes = (size_t)width * height * 2;

    /* 2. 输出格式（JPEG） */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "S_FMT capture failed: errno %d", errno);
        goto fail;
    }

    /* 3. OUTPUT 缓冲：USERPTR（数据源由调用者提供） */
    struct v4l2_requestbuffers req = { 0 };
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS output failed: errno %d", errno);
        goto fail;
    }

    /* 4. CAPTURE 缓冲：MMAP 一块 */
    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "REQBUFS capture failed: errno %d", errno);
        goto fail;
    }

    struct v4l2_buffer buf = { 0 };
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    if (xioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) {
        ESP_LOGE(TAG, "QUERYBUF capture failed: errno %d", errno);
        goto fail;
    }
    s_jp.cap_buf = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    if (s_jp.cap_buf == MAP_FAILED) {
        s_jp.cap_buf = NULL;
        ESP_LOGE(TAG, "mmap capture failed: errno %d", errno);
        goto fail;
    }
    s_jp.cap_len = buf.length;
    if (xioctl(fd, VIDIOC_QBUF, &buf) != 0) {
        ESP_LOGE(TAG, "QBUF capture failed: errno %d", errno);
        goto fail;
    }
    s_jp.cap_queued = true;

    /* 5. 开流 */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON capture failed: errno %d", errno);
        goto fail;
    }
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (xioctl(fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "STREAMON output failed: errno %d", errno);
        goto fail;
    }

    s_jp.fd = fd;
    s_jp.width = width;
    s_jp.height = height;
    s_jp.streaming = true;

    if (set_quality(fd, JPEG_QUALITY) == ESP_OK) {
        ESP_LOGI(TAG, "jpeg ready %dx%d, quality=%d, cap_buf=%u KB",
                 width, height, JPEG_QUALITY, (unsigned)(s_jp.cap_len / 1024));
    } else {
        ESP_LOGW(TAG, "jpeg ready %dx%d, quality set FAILED (default 80)", width, height);
    }
    return ESP_OK;

fail:
    if (s_jp.cap_buf) {
        munmap(s_jp.cap_buf, s_jp.cap_len);
        s_jp.cap_buf = NULL;
    }
    close(fd);
    return ESP_FAIL;
}

int app_jpeg_encode_rgb565(const uint8_t *rgb565, size_t len, const uint8_t **out)
{
    jpeg_enc_t *jp = &s_jp;
    if (jp->fd < 0 || !out || !rgb565) {
        ESP_LOGE(TAG, "encode: encoder not ready");
        return -1;
    }
    if (len < jp->src_bytes) {
        ESP_LOGE(TAG, "encode: src too short %u < %u", (unsigned)len, (unsigned)jp->src_bytes);
        return -1;
    }
    len = jp->src_bytes;

    /* 上一张码流还没归还驱动时先归还 */
    if (!jp->cap_queued) {
        struct v4l2_buffer c = { 0 };
        c.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        c.memory = V4L2_MEMORY_MMAP;
        c.index = 0;
        xioctl(jp->fd, VIDIOC_QBUF, &c);
        jp->cap_queued = true;
    }

    /* 投喂一帧（用户指针，零拷贝） */
    struct v4l2_buffer obuf = { 0 };
    obuf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    obuf.memory = V4L2_MEMORY_USERPTR;
    obuf.index = 0;
    obuf.m.userptr = (unsigned long)rgb565;
    obuf.length = len;
    obuf.bytesused = len;
    if (xioctl(jp->fd, VIDIOC_QBUF, &obuf) != 0) {
        ESP_LOGE(TAG, "QBUF output failed: errno %d", errno);
        return -1;
    }

    /* 取码流 */
    struct v4l2_buffer cbuf = { 0 };
    cbuf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    cbuf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(jp->fd, VIDIOC_DQBUF, &cbuf) != 0) {
        ESP_LOGE(TAG, "DQBUF capture failed: errno %d", errno);
        return -1;
    }
    jp->cap_queued = false;     /* 码流缓冲在调用者手里，下次编码前归还 */

    /* 回收输出缓冲（必须！否则下一次 QBUF 没有空闲缓冲） */
    struct v4l2_buffer odone = { 0 };
    odone.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    odone.memory = V4L2_MEMORY_USERPTR;
    if (xioctl(jp->fd, VIDIOC_DQBUF, &odone) != 0) {
        ESP_LOGW(TAG, "DQBUF output failed: errno %d", errno);
    }

    if (cbuf.bytesused == 0 || cbuf.bytesused > jp->cap_len) {
        ESP_LOGE(TAG, "bad jpeg size %u (cap %u)", (unsigned)cbuf.bytesused, (unsigned)jp->cap_len);
        return -1;
    }
    *out = jp->cap_buf;
    return (int)cbuf.bytesused;
}

void app_jpeg_deinit(void)
{
    if (s_jp.fd < 0) {
        return;
    }
    enum v4l2_buf_type type;
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_jp.fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(s_jp.fd, VIDIOC_STREAMOFF, &type);
    if (s_jp.cap_buf) {
        munmap(s_jp.cap_buf, s_jp.cap_len);
        s_jp.cap_buf = NULL;
    }
    close(s_jp.fd);
    s_jp.fd = -1;
    s_jp.width = s_jp.height = 0;
    s_jp.streaming = false;
}
