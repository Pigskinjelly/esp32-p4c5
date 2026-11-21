/*
 * 轻量 RTSP + RTP/H.264 服务器实现
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"

#include "app_encoder.h"
#include "rtsp_server.h"

static const char *TAG = "rtsp";

#define RTSP_QUEUE_DEPTH        6
#define RTSP_MAX_FRAME_SIZE     (192 * 1024)
#define RTP_MTU                 1400
#define RTP_PAYLOAD_TYPE        96
#define RTSP_SESSION_ID         "20261111"

typedef struct {
    uint8_t *data;
    size_t   len;
    bool     key;
    uint64_t pts_us;
} frame_item_t;

typedef struct {
    int fd;                     /* RTSP 控制连接 */
    bool playing;

    /* 传输 */
    int  transport;             /* 0=UDP, 1=TCP interleaved */
    struct sockaddr_in rtp_addr;
    int  rtp_sock;
    int  interleaved_rtp;       /* TCP 通道号 */
    int  interleaved_rtcp;

    /* RTP 状态 */
    uint16_t seq;
    uint32_t ssrc;
    uint32_t timestamp;

    /* 发送缓冲 */
    SemaphoreHandle_t tx_lock;
} rtsp_session_t;

static frame_item_t s_queue[RTSP_QUEUE_DEPTH];
static int s_q_head, s_q_tail, s_q_count;
static SemaphoreHandle_t s_q_lock;

#define RTSP_MAX_CLIENTS 4       /* 并发连接上限（每连接 ≈7KB 内部栈） */
#define RTSP_MAX_RTCP    2048    /* interleaved RTCP 最大长度 */

static volatile bool s_running;
static volatile int s_clients;
static uint32_t s_pkts;
static uint32_t s_bytes;

static const uint8_t *s_sps;
static size_t s_sps_len;
static const uint8_t *s_pps;
static size_t s_pps_len;

/* ---------------- 帧队列 ---------------- */

static void queue_reset(void)
{
    xSemaphoreTake(s_q_lock, portMAX_DELAY);
    for (int i = 0; i < RTSP_QUEUE_DEPTH; i++) {
        if (s_queue[i].data) {
            free(s_queue[i].data);
            s_queue[i].data = NULL;
        }
    }
    s_q_head = s_q_tail = s_q_count = 0;
    xSemaphoreGive(s_q_lock);
}

static void queue_push(const uint8_t *data, size_t len, bool key, uint64_t pts_us)
{
    if (len == 0 || len > RTSP_MAX_FRAME_SIZE) {
        return;
    }

    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!copy) {
        ESP_LOGW(TAG, "no mem for frame (%u)", (unsigned)len);
        return;
    }
    memcpy(copy, data, len);

    xSemaphoreTake(s_q_lock, portMAX_DELAY);

    if (s_q_count == RTSP_QUEUE_DEPTH) {
        /* 丢最旧一帧，保证低延迟 */
        if (s_queue[s_q_tail].data) {
            free(s_queue[s_q_tail].data);
        }
        s_queue[s_q_tail].data = NULL;
        s_q_tail = (s_q_tail + 1) % RTSP_QUEUE_DEPTH;
        s_q_count--;
    }

    s_queue[s_q_head].data = copy;
    s_queue[s_q_head].len = len;
    s_queue[s_q_head].key = key;
    s_queue[s_q_head].pts_us = pts_us;
    s_q_head = (s_q_head + 1) % RTSP_QUEUE_DEPTH;
    s_q_count++;

    xSemaphoreGive(s_q_lock);
}

/* 取出最旧一帧，数据所有权转给调用者（需 free） */
static bool queue_pop(uint8_t **data, size_t *len, bool *key, uint64_t *pts_us)
{
    bool ok = false;

    xSemaphoreTake(s_q_lock, portMAX_DELAY);
    if (s_q_count > 0) {
        *data = s_queue[s_q_tail].data;
        *len = s_queue[s_q_tail].len;
        *key = s_queue[s_q_tail].key;
        *pts_us = s_queue[s_q_tail].pts_us;
        s_queue[s_q_tail].data = NULL;
        s_q_tail = (s_q_tail + 1) % RTSP_QUEUE_DEPTH;
        s_q_count--;
        ok = true;
    }
    xSemaphoreGive(s_q_lock);

    return ok;
}

/* ---------------- base64（SDP 用） ---------------- */

static int b64_encode(const uint8_t *src, size_t slen, char *out, size_t olen)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((slen + 2) / 3) * 4 + 1;

    if (olen < need) {
        return -1;
    }

    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        uint32_t v = src[i] << 16;
        if (i + 1 < slen) {
            v |= src[i + 1] << 8;
        }
        if (i + 2 < slen) {
            v |= src[i + 2];
        }
        out[o++] = tbl[(v >> 18) & 0x3F];
        out[o++] = tbl[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < slen) ? tbl[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < slen) ? tbl[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return (int)o;
}

/* ---------------- RTP 发送 ---------------- */

static void rtp_write(rtsp_session_t *s, const uint8_t *pkt, size_t len)
{
    if (s->transport == 0) {
        if (s->rtp_sock >= 0) {
            sendto(s->rtp_sock, pkt, len, 0, (struct sockaddr *)&s->rtp_addr, sizeof(s->rtp_addr));
        }
    } else {
        /* RTP over RTSP/TCP: '$' + channel + 2 字节长度 + 数据 */
        uint8_t hdr[4] = { '$', (uint8_t)s->interleaved_rtp,
                           (uint8_t)((len >> 8) & 0xFF), (uint8_t)(len & 0xFF) };
        xSemaphoreTake(s->tx_lock, portMAX_DELAY);
        if (send(s->fd, hdr, 4, 0) == 4) {
            send(s->fd, pkt, len, 0);
        }
        xSemaphoreGive(s->tx_lock);
    }
    s_pkts++;
    s_bytes += len;
}

static void rtp_send_nal(rtsp_session_t *s, const uint8_t *nal, size_t len, bool last_of_frame)
{
    uint8_t pkt[RTP_MTU + 64];
    bool marker = last_of_frame;

    if (len <= RTP_MTU) {
        /* 单 NAL 单元 */
        pkt[0] = 0x80;
        pkt[1] = (marker ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE;
        pkt[2] = (s->seq >> 8) & 0xFF;
        pkt[3] = s->seq & 0xFF;
        pkt[4] = (s->timestamp >> 24) & 0xFF;
        pkt[5] = (s->timestamp >> 16) & 0xFF;
        pkt[6] = (s->timestamp >> 8) & 0xFF;
        pkt[7] = s->timestamp & 0xFF;
        pkt[8] = (s->ssrc >> 24) & 0xFF;
        pkt[9] = (s->ssrc >> 16) & 0xFF;
        pkt[10] = (s->ssrc >> 8) & 0xFF;
        pkt[11] = s->ssrc & 0xFF;
        memcpy(&pkt[12], nal, len);
        s->seq++;
        rtp_write(s, pkt, 12 + len);
        return;
    }

    /* FU-A 分片 */
    const size_t max_payload = RTP_MTU - 2;
    uint8_t nal_hdr = nal[0];
    uint8_t fu_ind = (nal_hdr & 0xE0) | 28;         /* F|NRI | FU-A */
    uint8_t type = nal_hdr & 0x1F;
    size_t offset = 1;

    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > max_payload) {
            chunk = max_payload;
        }
        bool start = (offset == 1);
        bool end = (offset + chunk >= len);

        pkt[0] = 0x80;
        pkt[1] = ((marker && end) ? 0x80 : 0x00) | RTP_PAYLOAD_TYPE;
        pkt[2] = (s->seq >> 8) & 0xFF;
        pkt[3] = s->seq & 0xFF;
        pkt[4] = (s->timestamp >> 24) & 0xFF;
        pkt[5] = (s->timestamp >> 16) & 0xFF;
        pkt[6] = (s->timestamp >> 8) & 0xFF;
        pkt[7] = s->timestamp & 0xFF;
        pkt[8] = (s->ssrc >> 24) & 0xFF;
        pkt[9] = (s->ssrc >> 16) & 0xFF;
        pkt[10] = (s->ssrc >> 8) & 0xFF;
        pkt[11] = s->ssrc & 0xFF;
        pkt[12] = fu_ind;
        pkt[13] = (start ? 0x80 : 0x00) | (end ? 0x40 : 0x00) | type;
        memcpy(&pkt[14], nal + offset, chunk);

        s->seq++;
        rtp_write(s, pkt, 14 + chunk);
        offset += chunk;
    }
}

/* 把一整帧 Annex-B 码流按 NAL 拆开依次发送 */
static void rtp_send_frame(rtsp_session_t *s, const uint8_t *data, size_t len, uint64_t pts_us)
{
    s->timestamp = (uint32_t)(pts_us * 9 / 100);   /* 90kHz */

    size_t i = 0;
    while (i + 3 < len) {
        if (data[i] != 0x00 || data[i + 1] != 0x00) {
            i++;
            continue;
        }
        size_t sc;
        if (data[i + 2] == 0x01) {
            sc = 3;
        } else if (data[i + 2] == 0x00 && data[i + 3] == 0x01) {
            sc = 4;
        } else {
            i++;
            continue;
        }

        size_t start = i + sc;
        if (start >= len) {
            break;
        }

        size_t j = start + 1;
        while (j + 3 < len) {
            if (data[j] == 0x00 && data[j + 1] == 0x00 &&
                (data[j + 2] == 0x01 || (data[j + 2] == 0x00 && data[j + 3] == 0x01))) {
                break;
            }
            j++;
        }

        size_t nal_len = j - start;
        if (nal_len > 0) {
            bool last = (j + 3 >= len);
            rtp_send_nal(s, data + start, nal_len, last);
        }
        i = j;
    }
}

/* ---------------- RTSP 协议 ---------------- */

static int rtsp_read_request(int fd, char *buf, int cap)
{
    int total = 0;

    while (total < cap - 1) {
        int n = recv(fd, buf + total, 1, 0);
        if (n <= 0) {
            return -1;
        }
        total += n;
        if (total >= 4 &&
            buf[total - 4] == '\r' && buf[total - 3] == '\n' &&
            buf[total - 2] == '\r' && buf[total - 1] == '\n') {
            break;
        }
    }
    buf[total] = '\0';
    return total;
}

static const char *rtsp_get_header(const char *req, const char *name, char *out, size_t olen)
{
    size_t nlen = strlen(name);
    const char *p = req;

    while ((p = strstr(p, "\r\n")) != NULL) {
        p += 2;
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            while (*v == ' ') {
                v++;
            }
            const char *e = strstr(v, "\r\n");
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l >= olen) {
                l = olen - 1;
            }
            memcpy(out, v, l);
            out[l] = '\0';
            return out;
        }
    }
    return NULL;
}

static void rtsp_send_response(rtsp_session_t *s, int code, const char *cseq,
                               const char *extra, const char *body)
{
    char head[1024];
    char clen[64] = { 0 };
    int body_len = body ? (int)strlen(body) : 0;

    if (body_len > 0) {
        snprintf(clen, sizeof(clen), "Content-Length: %d\r\n", body_len);
    }

    const char *reason = (code == 200) ? "OK" : (code == 404 ? "Not Found" : "Error");

    /* 头部必须以空行结束，Content-Length 必须在这之前 */
    int n = snprintf(head, sizeof(head),
                     "RTSP/1.0 %d %s\r\n"
                     "CSeq: %s\r\n"
                     "Server: ESP32-P4C5\r\n"
                     "%s"
                     "%s"
                     "%s"
                     "\r\n",
                     code, reason,
                     cseq ? cseq : "0",
                     extra ? extra : "",
                     body_len ? "Content-Type: application/sdp\r\n" : "",
                     clen);

    xSemaphoreTake(s->tx_lock, portMAX_DELAY);
    send(s->fd, head, n, 0);
    if (body_len > 0) {
        send(s->fd, body, body_len, 0);
    }
    xSemaphoreGive(s->tx_lock);
}

static void rtsp_build_sdp(char *sdp, size_t cap)
{
    char sps_b64[160] = { 0 };
    char pps_b64[160] = { 0 };

    if (s_sps && s_sps_len) {
        b64_encode(s_sps, s_sps_len, sps_b64, sizeof(sps_b64));
    }
    if (s_pps && s_pps_len) {
        b64_encode(s_pps, s_pps_len, pps_b64, sizeof(pps_b64));
    }

    esp_netif_ip_info_t ip = { 0 };
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_get_ip_info(netif, &ip);
    }

    char fmtp[256];
    if (sps_b64[0] && pps_b64[0]) {
        snprintf(fmtp, sizeof(fmtp),
                 "a=fmtp:%d packetization-mode=1;profile-level-id=42e01f;"
                 "sprop-parameter-sets=%s,%s\r\n",
                 RTP_PAYLOAD_TYPE, sps_b64, pps_b64);
    } else {
        /* SPS/PPS 尚未就绪：不写 sprop，解码器从码流内获取 */
        snprintf(fmtp, sizeof(fmtp),
                 "a=fmtp:%d packetization-mode=1;profile-level-id=42e01f\r\n",
                 RTP_PAYLOAD_TYPE);
    }

    snprintf(sdp, cap,
             "v=0\r\n"
             "o=- 0 0 IN IP4 " IPSTR "\r\n"
             "s=ESP32-P4C5 IP Camera\r\n"
             "c=IN IP4 " IPSTR "\r\n"
             "t=0 0\r\n"
             "a=control:*\r\n"
             "m=video 0 RTP/AVP %d\r\n"
             "a=rtpmap:%d H264/90000\r\n"
             "%s"
             "a=control:track1\r\n",
             IP2STR(&ip.ip), IP2STR(&ip.ip),
             RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE, fmtp);
}

static void rtsp_handle_setup(rtsp_session_t *s, const char *req, const char *cseq)
{
    char transport[256] = { 0 };
    char extra[320] = { 0 };

    rtsp_get_header(req, "Transport", transport, sizeof(transport));

    if (strstr(transport, "TCP") || strstr(transport, "tcp")) {
        /* interleaved */
        int ch0 = 0, ch1 = 1;
        const char *p = strstr(transport, "interleaved=");
        if (p) {
            sscanf(p + 12, "%d-%d", &ch0, &ch1);
        }
        s->transport = 1;
        s->interleaved_rtp = ch0;
        s->interleaved_rtcp = ch1;
        snprintf(extra, sizeof(extra),
                 "Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
                 "Session: %s;timeout=60\r\n",
                 ch0, ch1, RTSP_SESSION_ID);
    } else {
        int port = 0;
        const char *p = strstr(transport, "client_port=");
        if (p) {
            sscanf(p + 12, "%d", &port);
        }
        if (port <= 0) {
            port = 5000;
        }

        if (s->rtp_sock >= 0) {
            close(s->rtp_sock);
        }
        s->rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        s->transport = 0;
        s->rtp_addr.sin_family = AF_INET;
        s->rtp_addr.sin_port = htons(port);

        snprintf(extra, sizeof(extra),
                 "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
                 "Session: %s;timeout=60\r\n",
                 port, port + 1, port + 2, port + 3, RTSP_SESSION_ID);
    }

    /* 客户端 IP 从控制连接获取 */
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(s->fd, (struct sockaddr *)&peer, &plen) == 0) {
        s->rtp_addr.sin_addr = peer.sin_addr;
    }

    rtsp_send_response(s, 200, cseq, extra, NULL);
}

static void rtsp_handle_request(rtsp_session_t *s, const char *req)
{
    char cseq[32] = "0";
    rtsp_get_header(req, "CSeq", cseq, sizeof(cseq));

    if (strncmp(req, "OPTIONS", 7) == 0) {
        rtsp_send_response(s, 200, cseq,
                           "Public: OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER\r\n",
                           NULL);
    } else if (strncmp(req, "DESCRIBE", 8) == 0) {
        char sdp[768];
        rtsp_build_sdp(sdp, sizeof(sdp));
        rtsp_send_response(s, 200, cseq, NULL, sdp);
    } else if (strncmp(req, "SETUP", 5) == 0) {
        rtsp_handle_setup(s, req, cseq);
    } else if (strncmp(req, "PLAY", 4) == 0) {
        rtsp_send_response(s, 200, cseq,
                           "Session: " RTSP_SESSION_ID ";timeout=60\r\n"
                           "Range: npt=0.000-\r\n",
                           NULL);
        s->playing = true;
        ESP_LOGI(TAG, "PLAY: streaming start");
    } else if (strncmp(req, "PAUSE", 5) == 0) {
        s->playing = false;
        rtsp_send_response(s, 200, cseq, "Session: " RTSP_SESSION_ID "\r\n", NULL);
    } else if (strncmp(req, "TEARDOWN", 8) == 0) {
        s->playing = false;
        rtsp_send_response(s, 200, cseq, "Session: " RTSP_SESSION_ID "\r\n", NULL);
        ESP_LOGI(TAG, "TEARDOWN");
    } else if (strncmp(req, "GET_PARAMETER", 13) == 0) {
        rtsp_send_response(s, 200, cseq, "Session: " RTSP_SESSION_ID "\r\n", NULL);
    } else {
        rtsp_send_response(s, 200, cseq, NULL, NULL);
    }
}

/* ---------------- 会话任务 ---------------- */

static void rtsp_session_task(void *arg)
{
    rtsp_session_t *s = (rtsp_session_t *)arg;
    char *req = malloc(2048);

    if (!req) {
        goto out;
    }

    while (s_running) {
        int n = rtsp_read_request(s->fd, req, 2048);
        if (n <= 0) {
            break;
        }
        rtsp_handle_request(s, req);

        if (!s->playing) {
            continue;
        }

        /* 播放循环：取帧发送，同时兼顾控制连接 */
        while (s->playing && s_running) {
            fd_set rfds;
            struct timeval tv = { .tv_sec = 0, .tv_usec = 20000 };
            FD_ZERO(&rfds);
            FD_SET(s->fd, &rfds);

            int r = select(s->fd + 1, &rfds, NULL, NULL, &tv);
            if (r > 0) {
                /* 可能有 TEARDOWN 等控制命令 */
                char peek[4];
                int pn = recv(s->fd, peek, 1, MSG_DONTWAIT | MSG_PEEK);
                if (pn == 0) {
                    break;      /* 对端关闭 */
                }
                if (pn > 0 && peek[0] != '$') {
                    break;      /* 回到上层读请求 */
                }
                /* '$' 开头的 interleaved RTCP，丢弃
                 * R-07：长度来自报文，最大 65535；RTCP 不可能这么大，
                 * 直接断开这个连接，别按攻击者给的长度 malloc。 */
                if (pn > 0) {
                    uint8_t tmp[4];
                    if (recv(s->fd, tmp, 4, MSG_DONTWAIT) == 4) {
                        int len = (tmp[2] << 8) | tmp[3];
                        if (len <= 0 || len > RTSP_MAX_RTCP) {
                            ESP_LOGW(TAG, "bad interleaved len %d, drop client", len);
                            break;
                        }
                        uint8_t *drop = malloc(len);
                        if (drop) {
                            recv(s->fd, drop, len, MSG_DONTWAIT);
                            free(drop);
                        }
                    }
                }
            }

            uint8_t *data = NULL;
            size_t len = 0;
            bool key = false;
            uint64_t pts = 0;

            if (queue_pop(&data, &len, &key, &pts)) {
                rtp_send_frame(s, data, len, pts);
                free(data);
            }
        }
    }

    free(req);

out:
    s->playing = false;
    if (s->rtp_sock >= 0) {
        close(s->rtp_sock);
        s->rtp_sock = -1;
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    if (s->tx_lock) {
        vSemaphoreDelete(s->tx_lock);
    }
    free(s);
    s_clients--;
    ESP_LOGI(TAG, "session closed");
    vTaskDelete(NULL);
}

static void rtsp_listen_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind %d failed: %d", port, errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 2) != 0) {
        ESP_LOGE(TAG, "listen failed: %d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RTSP server listening on %d, url rtsp://<ip>:%d/%s",
             port, port, RTSP_STREAM_PATH);

    while (s_running) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);

        if (fd < 0) {
            if (s_running) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }

        /* R-07：限制并发连接数，防止 10 个连接把内部堆吃干 */
        if (s_clients >= RTSP_MAX_CLIENTS) {
            ESP_LOGW(TAG, "too many clients (%d), reject %s:%d",
                     s_clients, inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
            close(fd);
            continue;
        }

        ESP_LOGI(TAG, "client %s:%d connected", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));

        rtsp_session_t *s = calloc(1, sizeof(rtsp_session_t));
        if (!s) {
            close(fd);
            continue;
        }

        s->fd = fd;
        s->rtp_sock = -1;
        s->transport = 0;
        s->seq = (uint16_t)(esp_timer_get_time() & 0xFFFF);
        s->ssrc = (uint32_t)esp_timer_get_time();
        s->tx_lock = xSemaphoreCreateMutex();
        s_clients++;

        if (xTaskCreate(rtsp_session_task, "rtsp_sess", 6144, s, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "create session task failed");
            close(fd);
            free(s);
            s_clients--;
        }
    }

    close(listen_fd);
    vTaskDelete(NULL);
}

/* ---------------- 公共接口 ---------------- */

esp_err_t rtsp_server_start(int port)
{
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_q_lock = xSemaphoreCreateMutex();
    if (!s_q_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_q_head = s_q_tail = s_q_count = 0;
    s_running = true;

    if (xTaskCreate(rtsp_listen_task, "rtsp_listen", 4096, (void *)(intptr_t)port, 4, NULL) != pdPASS) {
        s_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t rtsp_server_push_frame(const uint8_t *data, size_t len, bool keyframe, uint64_t pts_us)
{
    if (!s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 没有客户端在播放就不占内存 */
    if (s_clients <= 0) {
        return ESP_OK;
    }

    /* 首次拿到的 SPS/PPS 缓存下来给 SDP 用 */
    if (!s_sps || !s_pps) {
        app_encoder_get_sps(&s_sps, &s_sps_len);
        app_encoder_get_pps(&s_pps, &s_pps_len);
    }

    queue_push(data, len, keyframe, pts_us);
    return ESP_OK;
}

bool rtsp_server_has_client(void)
{
    return s_clients > 0;
}

void rtsp_server_get_stats(uint32_t *pkts, uint32_t *bytes, int *clients)
{
    if (pkts) {
        *pkts = s_pkts;
    }
    if (bytes) {
        *bytes = s_bytes;
    }
    if (clients) {
        *clients = s_clients;
    }
}

void rtsp_server_stop(void)
{
    s_running = false;
    vTaskDelay(pdMS_TO_TICKS(200));
    queue_reset();
    if (s_q_lock) {
        vSemaphoreDelete(s_q_lock);
        s_q_lock = NULL;
    }
}
