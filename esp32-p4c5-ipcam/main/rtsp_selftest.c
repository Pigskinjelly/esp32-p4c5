#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "rtsp_selftest.h"

static const char *TAG = "selftest";

#define SELFTEST_BUF 1024

static int send_req(int fd, const char *req)
{
    int n = send(fd, req, strlen(req), 0);
    return (n == (int)strlen(req)) ? 0 : -1;
}

/* 读到响应头结束（\r\n\r\n）或超时 */
static int recv_response(int fd, char *buf, int cap)
{
    int total = 0;
    while (total < cap - 1) {
        int n = recv(fd, buf + total, cap - 1 - total, 0);
        if (n <= 0) {
            break;
        }
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) {
            break;
        }
    }
    return total;
}

static void selftest_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(6000));

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "connect 127.0.0.1:%d failed: %d", port, errno);
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    char *rbuf = malloc(SELFTEST_BUF);
    if (!rbuf) {
        close(fd);
        vTaskDelete(NULL);
        return;
    }

    char url[64];
    snprintf(url, sizeof(url), "rtsp://127.0.0.1:%d/stream", port);
    char req[320];

    /* OPTIONS */
    snprintf(req, sizeof(req),
             "OPTIONS %s RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: selftest\r\n\r\n", url);
    int ok = (send_req(fd, req) == 0) && recv_response(fd, rbuf, SELFTEST_BUF) > 0;
    ESP_LOGI(TAG, "OPTIONS  -> %s", ok ? "ok" : "FAIL");
    if (!ok) {
        goto done;
    }

    /* DESCRIBE */
    snprintf(req, sizeof(req),
             "DESCRIBE %s RTSP/1.0\r\nCSeq: 2\r\nAccept: application/sdp\r\n\r\n", url);
    ok = (send_req(fd, req) == 0) && recv_response(fd, rbuf, SELFTEST_BUF) > 0;
    ESP_LOGI(TAG, "DESCRIBE -> %s%s", ok ? "ok" : "FAIL",
             (ok && strstr(rbuf, "m=video")) ? " (SDP 含 m=video)" : "");
    if (!ok) {
        goto done;
    }

    /* SETUP：先用 TCP interleaved，单 socket 最简单 */
    snprintf(req, sizeof(req),
             "SETUP %s/track1 RTSP/1.0\r\nCSeq: 3\r\n"
             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n", url);
    ok = (send_req(fd, req) == 0) && recv_response(fd, rbuf, SELFTEST_BUF) > 0;
    ESP_LOGI(TAG, "SETUP    -> %s", ok ? "ok" : "FAIL");
    if (!ok) {
        goto done;
    }

    /* PLAY */
    snprintf(req, sizeof(req), "PLAY %s RTSP/1.0\r\nCSeq: 4\r\nSession: 0\r\n\r\n", url);
    ok = (send_req(fd, req) == 0);
    ESP_LOGI(TAG, "PLAY     -> %s", ok ? "sent" : "FAIL");
    if (!ok) {
        goto done;
    }
    recv_response(fd, rbuf, SELFTEST_BUF);

    /* 统计 3 秒内的 interleaved RTP 包 */
    {
        int pkts = 0;
        long bytes = 0;
        TickType_t t0 = xTaskGetTickCount();
        while ((xTaskGetTickCount() - t0) < pdMS_TO_TICKS(3000)) {
            uint8_t hdr[4];
            int n = recv(fd, hdr, 4, 0);
            if (n != 4) {
                continue;
            }
            if (hdr[0] != '$') {
                continue;
            }
            int len = (hdr[2] << 8) | hdr[3];
            int got = 0;
            while (got < len) {
                int m = recv(fd, rbuf, (len - got) > SELFTEST_BUF ? SELFTEST_BUF : (len - got), 0);
                if (m <= 0) {
                    break;
                }
                got += m;
            }
            if (hdr[1] == 0) {   /* channel 0 = RTP */
                pkts++;
                bytes += got;
            }
        }
        if (pkts > 0) {
            ESP_LOGI(TAG, "RTP 收到 %d 包 / %ld 字节 -> RTSP 出流正常", pkts, bytes);
        } else {
            ESP_LOGE(TAG, "3 秒内没收到 RTP 包 -> 服务端未出流");
        }
    }

done:
    free(rbuf);
    close(fd);
    vTaskDelete(NULL);
}

esp_err_t rtsp_selftest_start(int port, int delay_ms)
{
    BaseType_t r = xTaskCreate(selftest_task, "rtsp_self", 6144, (void *)(intptr_t)port, 4, NULL);
    return (r == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}
