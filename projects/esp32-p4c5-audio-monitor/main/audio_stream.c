/*
 * Web 服务 + WebSocket 音频推流实现。
 * 采集任务：audio_capture_read() 读 PCM -> WebSocket 广播给所有客户端。
 * 前端：Web Audio API 实时播放 16kHz/16bit 单声道 PCM。
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "audio_capture.h"
#include "audio_stream.h"

static const char *TAG = "audio_stream";

static httpd_handle_t s_server = NULL;

/* 内嵌前端页面：WebSocket + Web Audio 播放 */
static const char *INDEX_HTML =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-P4C5 远程监听</title>"
    "<style>body{font-family:system-ui;max-width:480px;margin:40px auto;padding:0 16px;"
    "text-align:center}#btn{font-size:18px;padding:12px 28px;border:none;border-radius:8px;"
    "background:#1677ff;color:#fff}#status{margin:16px 0;color:#666}</style></head><body>"
    "<h2>ESP32-P4C5 远程音频监听</h2>"
    "<div id='status'>未连接</div>"
    "<button id='btn' onclick='toggle()'>开始监听</button>"
    "<script>"
    "let ws=null,ctx=null,node=null,queue=[],playing=false;"
    "const SR=16000,BLK=4096;"
    "function toggle(){if(playing){stop();}else{start();}}"
    "function start(){ws=new WebSocket('ws://'+location.host+'/ws');ws.binaryType='arraybuffer';"
    "ws.onopen=()=>{status.textContent='已连接';if(!ctx){ctx=new AudioContext({sampleRate:SR});"
    "node=ctx.createScriptProcessor(BLK,1,1);node.onaudioprocess=fill;node.connect(ctx.destination);}"
    "playing=true;btn.textContent='停止监听';};"
    "ws.onmessage=(e)=>{queue.push(new Int16Array(e.data));};"
    "ws.onclose=()=>{status.textContent='连接断开';};}"
    "function fill(ev){let out=ev.outputBuffer.getChannelData(0);let i=0;"
    "while(i<out.length){if(queue.length===0){out[i++]=0;continue;}let b=queue[0];"
    "let n=Math.min(b.length,out.length-i);for(let j=0;j<n;j++){out[i+j]=b[j]/32768.0;}"
    "i+=n;if(n===b.length){queue.shift();}else{queue[0]=b.subarray(n);}}}"
    "function stop(){if(ws){ws.close();}playing=false;btn.textContent='开始监听';"
    "status.textContent='已停止';}"
    "</script></body></html>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "websocket client connected");
        return ESP_OK;
    }

    /* 其余帧（PING/PONG/CLOSE）交给默认处理，本 demo 客户端不向服务器发音频数据 */
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }
    if (ws_pkt.len) {
        buf = calloc(1, ws_pkt.len + 1);
        if (buf) {
            ws_pkt.payload = buf;
            httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            free(buf);
        }
    }
    return ESP_OK;
}

/* 采集任务：读 PCM 并广播给所有 WebSocket 客户端 */
static void stream_task(void *arg)
{
    const int frame_bytes = CONFIG_MONITOR_AUDIO_SAMPLE_RATE * 2 * 20 / 1000; /* 20ms */
    uint8_t *pcm = malloc(frame_bytes);
    if (pcm == NULL) {
        ESP_LOGE(TAG, "no mem for pcm buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        int r = audio_capture_read(pcm, frame_bytes);
        if (r <= 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        /* 获取所有已连接客户端 fd 并广播 */
        size_t fds = 8;
        int client_fds[8];
        if (httpd_get_client_list(s_server, &fds, client_fds) == ESP_OK) {
            for (size_t i = 0; i < fds; i++) {
                httpd_ws_frame_t ws_pkt = {
                    .final = true,
                    .fragmented = false,
                    .type = HTTPD_WS_TYPE_BINARY,
                    .payload = pcm,
                    .len = r,
                };
                httpd_ws_send_frame_async(s_server, client_fds[i], &ws_pkt);
            }
        }
    }
}

esp_err_t audio_stream_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;

    ESP_ERROR_CHECK(httpd_start(&s_server, &config));

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(s_server, &index_uri);

    httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };
    httpd_register_uri_handler(s_server, &ws_uri);

    xTaskCreate(stream_task, "audio_stream", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "web server started on port 80, /ws for audio stream");
    return ESP_OK;
}
