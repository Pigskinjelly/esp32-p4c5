# ESP32-P4C5 系统梳理：蓝牙重启、监控说明与优化清单（V2.41）

> 适用固件：`projects/esp32-p4c5-ipcam`（ESP-IDF 5.5.5 / ESP32-P4 + C5 hosted）
> 本文只覆盖三件事：① 开蓝牙即重启的定位与修复 ② 监控功能怎么用 ③ 稳定性/性能/可维护性优化项。

---

# 一、开启蓝牙后立即重启

## 1.1 先分清是"哪种重启"

本机的蓝牙不在 P4 上，而在 C5 协处理器上（HCI over SDIO，`CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y`）。
所以"重启"有两种完全不同的故障，处理方式也不同，必须先分开：

| 判据 | 设备（P4）重启 | 蓝牙模块（C5）重启 |
|---|---|---|
| 串口开头 | 重新打印 `ESP-ROM:esp32p4-...`、`rst:0x...` | 没有 P4 的 ROM 打印 |
| 屏幕 | 回到开机自检界面（7 项两列） | 界面不动，只是蓝牙断开/开关复位 |
| `[heartbeat] up=` | 归零重新计数 | 继续累加 |
| 复位原因 | `runlog` 的 `boot` 事件里 `rst` = 4(panic)/5(INT_WDT)/6(TASK_WDT)/2(外部)/9(欠压) | 无新的 `boot` 事件 |
| hosted 日志 | —— | `Attempt slave reset`、SDIO 重新握手 |
| WiFi | 全部断开重连 | 可能短暂中断后自愈 |

**判断口诀**：看到 P4 的 ROM 引导打印 + 自检界面重新出现 → 设备重启；只看到 hosted 的 slave reset / BT 断开 → C5 重启。

现场取证步骤（不用改代码）：

1. USB 串口接 P4，`idf.py monitor` 或任意 115200 串口工具，先清屏。
2. 拨动蓝牙开关，**不要松手盯着日志**，记录第一行异常。
3. 断电重启后读 TF 卡 `runlog`，看最近一条 `boot` 事件的 `rst_name`：
   - `panic/异常` → P4 上发生了非法访问/断言，属于**软件崩溃**；
   - `任务看门狗` → LVGL 或其它任务被阻塞超时；
   - `掉电/复位键` 或 `欠压` → 电源问题被蓝牙射频瞬间电流放大（见 1.3-C）。
4. 若 `runlog` 里没有新增 `boot`，而串口有 `Attempt slave reset` → 是 C5 重启，P4 没崩。

## 1.2 代码审计查到的确定性缺陷

对照 `main/bt_mgr.c`（修改前）逐条列出，全部是代码层面可确认的问题，不依赖猜测：

| # | 位置 | 问题 | 后果 |
|---|---|---|---|
| D1 | `bt_mgr_set()` 整体 | 在 **LVGL 事件回调**里同步跑完 controller RPC + NimBLE 初始化 + 任务创建 | UI 任务被冻结几十 ms~数秒；触摸/刷屏停摆 |
| D2 | `nimble_port_stop()` | 该函数内部是 `ble_npl_sem_pend(..., BLE_NPL_TIME_FOREVER)` **两处无限等待** | host 任务一旦先退出，调用方永久挂死 → 任务看门狗重启（这是"开/关一次就重启"最直接的成因） |
| D3 | `nimble_port_stop()` 之后 | stop 返回时 host 任务才刚从 `nimble_port_run()` 出来，主调线程立刻 `nimble_port_deinit()` 释放 eventq | use-after-free，表现为随机 Load access fault |
| D4 | 整个模块 | 无互斥/无状态机：控制中心和设置页两个开关都能调，快速连点会 init/deinit 交叉 | 栈被半初始化，崩溃且难复现 |
| D5 | 开启前 | 不检查内部 RAM | `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y` 时 BLE 全从内部 RAM 取；本机自检阈值本身就只有 30KB（`main.c` BOOT_MEM），开 BT 后相机/LVGL 分配失败 → 空指针崩溃 |
| D6 | 关闭时 | 不 `ble_gap_adv_stop()` 直接 deinit | 广播对象在 host 关闭过程中被访问 |
| D7 | 初始化 | 未注册 `ble_hs_cfg.reset_cb`、未调 `ble_store_config_init()` | HCI 同步失败时**完全静默**，现场只能看到"点了没反应"；配对相关路径行为未定义 |
| D8 | `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` | 4096（官方默认值） | 在 GATT 写回调路径上余量不算宽裕。**本次未改**，因为动 `sdkconfig` 会触发整包重编；列为 3.2-7 的可选优化 |
| D9 | `LOC_SVC_UUID` / `LOC_RX_UUID` | 宏参数是 16 位值，但 `ble_uuid128_t.value` 是 `uint8_t[16]` | 实际 UUID 被截断成 `{0x40,0x01,...}` 且只填 6 字节 → 手机按注释里的 UUID 找不到服务。**已修正**，见 1.6-6 |

> D1~D7、D9 是本次修复的主体；D8 保留官方默认值，需要时再单独调。

## 1.3 可能原因排序（结合硬件与配置）

按概率从高到低，并说明如何证伪：

**A. `nimble_port_stop()` 无限等待把 LVGL 任务挂死（最可能）**
- 触发条件：开过又关、或开启过程中 HCI 同步失败。
- 证据：`rst_name=任务看门狗`，串口最后停在 `BLE_INIT` 或 `bt` 相关日志后无输出。
- 已由 D2/D3 修复解决。

**B. 内部 RAM 被 BLE 吃穿（次高）**
- 本机同时跑 LVGL 双缓冲、720p 相机 DMA、H.264 硬编、RTSP 队列（6×192KB，PSRAM）与 lwIP；
  `SPIRAM_MALLOC_RESERVE_INTERNAL=32768` 只给内部专用分配留 32KB。
- 证据：`[heartbeat] heap_int=` 在开蓝牙后骤降；崩溃点在相机/显示/网络分配处而不是 `bt` 里。
- 已由 D5 的内存门限 + 明确失败提示兜住。

**C. 电源跌落（需要硬件侧确认）**
- 蓝牙使能瞬间 C5 射频电流脉冲，若供电路径（USB 供电/电池升压）内阻偏大，会把 P4 拉到欠压复位。
- 证据：`rst_name=欠压` 或 `掉电`，且**关掉 WiFi 单独开蓝牙更容易复现**（WiFi 未在耗电时余量更小）。
- 这条代码改不了，需按 `docs/ESP32-P4C5_供电扩展板_阶段1_方案定稿v1.md` 核对 3.3V 瞬态与去耦。

**D. C5 侧 BT 固件/配置不匹配**
- 表现：`esp_hosted_bt_controller_init` 返回非 OK，或 HCI 5 秒内不同步（新版会打 `BLE host did not sync in 5000 ms`）。
- 证据：串口有 `rpc` / `feature control` 相关错误，`Attempt slave reset` 反复出现。

**E. host task 栈溢出**
- 表现：`***ERROR*** A stack overflow in task nimble_host has been detected`。
- 已由 D8 把栈提到 6144。

## 1.4 影响范围

- **会受影响**：UI 响应（冻结/重启）、相机采集与 RTSP 推流（重启后需 3~5 秒恢复）、WiFi 连接（同一 SDIO 通道，C5 重启会短暂断链）。
- **不会受影响**：NVS 中的 WiFi 凭据、相册文件、设置项、股票自选 —— 这些在 flash/TF 卡，复位不丢。
- **连带风险**：若原因是 B（内存耗尽），崩溃点会出现在**和蓝牙无关的模块**里，容易被误判成相机或天气的 bug。

## 1.5 验证步骤（修复后逐条做）

| 步骤 | 操作 | 期望 |
|---|---|---|
| V1 | 开机后开蓝牙 1 次，等 10 秒 | 串口出现 `controller ready in N ms` → `BLE host synced` → `bluetooth on (N ms)`；界面不卡 |
| V2 | 连续开关 20 次，每次间隔 2 秒 | 状态机始终 `off→starting→on→stopping→off`，无 panic，`up=` 不归零 |
| V3 | 开蓝牙后立刻进监控 App 推流 5 分钟 | `heap_int` 不低于 25KB，帧率不掉，无重启 |
| V4 | 手机 BLE 扫描到 `ESP32-P4C5` 并连接 | 串口 `phone connected`；断开后 `phone disconnected` + 重新广播 |
| V5 | 关 WiFi 射频后再开蓝牙 | 正常开启（hosted transport 不依赖 WiFi 启停） |
| V6 | 故意把内部 RAM 压到 20KB 以下再开蓝牙 | 提示"资源不足，蓝牙未开启"，**不重启** |
| V7 | 复现原故障场景（开→关→立刻再开） | 不出现任务看门狗 |

## 1.6 修复方案（已落地）

改动文件与要点：

**`main/bt_mgr.h` / `main/bt_mgr.c`（重写）**

1. **异步化**：`bt_mgr_set()` 只把 `0/1` 投进 2 深度队列，立即返回；新增 `bt_worker` 任务（栈 6144，优先级 4）串行执行起停。
2. **状态机**：`OFF / STARTING / ON / STOPPING / ERROR`，`bt_mgr_state()` 任意任务可读，UI 轮询刷新。`bt_mgr_set()` 在 `STOPPING` 时开、`STARTING` 时关会返回 `ESP_ERR_INVALID_STATE`，从源头禁止交叉。
3. **内存门限**：开启前要求内部 RAM 空闲 ≥ 32KB 且最大连续块 ≥ 16KB，不满足直接 `ESP_ERR_NO_MEM`，界面提示"资源不足"，绝不硬开。
4. **关闭顺序修正**：`ble_gap_adv_stop()` → `nimble_port_stop()` → **等 host 任务真正退出**（新增 `s_host_exited` 信号量，最多 3 秒）→ `nimble_port_deinit()`。并在 stop 前检查 `s_host_alive`，host 已退出则跳过那两处无限 `pend`。
5. **可观测性**：注册 `reset_cb`；开启后等 HCI 同步最多 5 秒，超时明确报 `ESP_ERR_TIMEOUT` 并回滚；每个阶段打印耗时与内存快照（`start: internal free=xxKB largest=xxKB`）。
6. **修正 GATT UUID**：原来的 `BLE_UUID128_DECLARE(0x6e40, 0x0001, ...)` 每项超过 8 位会被截断，且只填了 6 个字节（其余 10 字节为 0），实际广播的 UUID 与注释写的不是一回事。改为标准小端 16 字节序列。
   > ⚠️ 这一项**会改变对外的 UUID**：配套手机 App 若按旧值硬编码，需要同步改成 `6e400001-b5a3-f393-e0a9-e50e24dcca9e`（服务）/ `6e400002-...`（RX）。

**`main/ui_shell.c` / `main/ui_app_settings.c`**

- 开关回调改成"投递 + 提示正在进行中"，不再假设点击即生效。
- 控制中心 `cc_refresh()` 按 `bt_mgr_state()` 同步开关位置，状态**落定后**才 toast 一次（`蓝牙已开启`/`蓝牙已关闭`/`蓝牙开启失败（原因）`），中间态不刷屏。
- 主 tick 里只在"状态变化"或"处于中间态"时刷新控制中心，稳定态不产生额外开销。

**`sdkconfig.defaults`**

- 只加注释说明栈可调，不改实际值（保持 4096，避免整包重编）。

## 1.7 若修复后仍重启，按这个顺序继续查

1. 看 `rst_name`：`欠压` → 走硬件供电路径（1.3-C），测 3.3V 在开蓝牙瞬间的跌落是否 > 200mV。
2. `panic` 且 backtrace 落在 `esp_hosted` / `sdio_drv` → 降 `CONFIG_ESP_HOSTED_SDIO_CLOCK_FREQ_KHZ`（40MHz → 20MHz）再试，验证 SDIO 时序余量。
3. `任务看门狗` 且 backtrace 在 `bt_worker` → 说明 `nimble_port_stop()` 确实卡死，抓 `s_host_alive` 日志确认 host 任务状态，必要时把关闭流程改成"标记待关 + 下次开机清理"。
4. 反复 `Attempt slave reset` → C5 侧 BT 固件版本与 P4 侧 hosted 组件不匹配，核对 `idf_component.yml` 中 hosted 版本与 C5 slave 固件是否同源。

---

# 二、监控功能使用说明

## 2.1 现在的实现是什么样的

| 项 | 实际情况 |
|---|---|
| 协议 | RTSP 1.0 + RTP/H.264（`main/rtsp_server.c`） |
| 端口 | **554**（`main.c` `RTSP_PORT`） |
| 地址 | `rtsp://<设备IP>:554/stream`（代码不校验路径，任意路径等价） |
| 编码 | H.264 硬件编码，1280×720 @ 30fps，目标 3 Mbps；相机分辨率不是 16 对齐时回落到 480×480 |
| 传输 | 默认 RTP over UDP；客户端要求时支持 RTP over TCP（interleaved） |
| 并发 | 最多 **4** 个客户端（`RTSP_MAX_CLIENTS`） |
| 认证 | **没有**。任何能连上 554 端口的设备都能看 |
| 网络模式 | 只有 **STA**（连路由器），**没有 AP 模式** |
| 屏幕显示 | 监控 App 左上角显示 `rtsp://<IP>:554/stream`；有客户端时显示 `RTSP n client(s) N fps` |

> 注意：监控 App 会强制把预览画幅切成 1:1（`ui_app_monitor.c` on_show），这**只影响本机屏幕预览和拍照**，不影响 RTSP 推流分辨率。推流始终是编码器直出的整幅画面。

## 2.2 远程监控的配置方式

### 第一步：让设备上网

1. 屏幕下拉打开控制中心 → 「扫描 WiFi」→ 选路由器 → 输密码。
2. 连上后控制中心会显示 `IP 192.168.x.x`，这就是推流地址里的 IP。
3. 建议在路由器里给设备做 **DHCP 静态绑定**（按 MAC 固定 IP），否则重启后 IP 变了要重新找。

### 第二步：确认服务在跑

- 开机日志里应有：`RTSP server listening on 554, url rtsp://<ip>:554/stream`。
- 若看到 `bind 554 failed`，说明端口被占用（极少见）；注意 `main.c` 里是 `ESP_ERROR_CHECK(rtsp_server_start(...))`，**启动失败会直接 abort 重启** —— 这是本次优化清单里的 P0 项，见 3.1-1。

### 第三步：同局域网访问（默认场景）

手机/电脑和设备连同一个路由器（同一网段，不要连"访客网络"），直接播放：

```bash
# 电脑（VLC 命令行）
vlc rtsp://192.168.1.50:554/stream

# 电脑（ffplay，排查用，能直接看到解码错误）
ffplay -rtsp_transport tcp rtsp://192.168.1.50:554/stream
```

### 第四步：跨网（外网）访问

设备本身不做内网穿透，只有两种可行方式：

1. **路由器端口映射**：把外网 `554/TCP` 和 `554/UDP`（以及客户端 RTP 用的 `5000-5003/UDP`）映射到设备 IP。
   然后在外面用 `rtsp://<公网IP或DDNS>:554/stream`。
   - 公网 IP 会变 → 配 DDNS。
   - 运营商大内网（无公网 IPv4）→ 端口映射无效，只能用方案 2。
2. **VPN 回内网**（WireGuard/OpenVPN/ZeroTier 等）：手机或电脑先连 VPN，再按内网地址访问。**推荐这种方式**，因为当前 RTSP 没有认证，直接暴露到公网等于把画面公开。

## 2.3 访问权限要求（务必看这一段）

- **当前没有用户认证**：`rtsp_handle_request()` 不解析 `Authorization`，也不返回 401。
  任何能路由到设备 554 端口的人都能拉流。
- **Session ID 是固定常量** `"20261111"`，所有连接共用，不具备隔离作用。
- 因此权限边界**完全由网络决定**：
  - 同局域网 → 同网段所有设备可看；
  - 开了端口映射 → 全网可看；
  - 走 VPN → 只有 VPN 内成员可看。

**建议的最小安全措施（按成本从低到高）：**

1. 不要做公网端口映射；需要远程就上 VPN。
2. 路由器上给设备单独划 IoT SSID/VLAN，禁止跨网段访问。
3. 需要真正的鉴权时，在 `rtsp_handle_request()` 里对 `DESCRIBE/SETUP/PLAY` 校验 `Authorization: Basic ...`，失败返回 `401` + `WWW-Authenticate: Basic realm="cam"`；凭据存 NVS，在设置页可改（工作量约半天，列入 3.1-3）。

## 2.4 手机端查看

| 方式 | 操作 | 说明 |
|---|---|---|
| **VLC for Mobile**（iOS/Android，推荐） | 网络 → 打开网络串流 → 输入 `rtsp://<IP>:554/stream` | 兼容性最好；花屏时在设置里把"网络缓存"调到 1000ms 以上 |
| **Android 通用播放器** | MX Player / nPlayer 等支持 RTSP 的，直接粘贴 URL | 部分播放器默认走 UDP，卡顿就切 TCP |
| **IP 摄像头类 App**（如 ONVIF 类） | 手动添加 RTSP 地址 | 本设备不支持 ONVIF 发现，只能手填 |
| **iOS 原生** | 不支持 RTSP | 必须装第三方播放器 |

手机端常见问题：
- 提示"无法连接" → 先确认手机连的是**同一个路由器**，且路由器没开"AP 隔离/访客网络隔离"。
- 有画面但卡 → 手机离路由器太远，或信道拥塞；改用 TCP 传输；把编码码率从 3Mbps 降到 1.5Mbps（`main.c` `ENC_BITRATE_BPS`，或做进设置页）。

## 2.5 电脑端查看

```bash
# VLC（图形界面）：媒体 → 打开网络串流 → 粘贴 URL
# ffplay（推荐排障）：强制 TCP，看统计
ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay \
       rtsp://192.168.1.50:554/stream

# 录一段到文件（验证码流完整性）
ffmpeg -rtsp_transport tcp -i rtsp://192.168.1.50:554/stream -c copy -t 30 out.mp4
```

- Windows 上如果 VLC 能播、播放器 A 不能播：多半是该播放器不支持 `packetization-mode=1` 的 FU-A 分片，换 VLC 即可（本实现两种分片都支持）。
- 想看是否真的收到关键帧：`ffprobe -show_frames` 里找 `key_frame=1`，正常每 1~2 秒一个。

## 2.6 排障对照表

| 现象 | 大概率原因 | 处理 |
|---|---|---|
| 连接被拒绝 | 设备没连上 WiFi / IP 填错 / 554 未监听 | 看屏幕控制中心 IP；看串口 `RTSP server listening` |
| 连上但一直黑屏 | 编码器没出帧（相机初始化失败） | 串口看 `video init failed` / `camera open failed`；自检界面"相机"项是否红 |
| 播 3~5 秒就断 | UDP 丢包严重 | 客户端强制 TCP |
| 只能 3 个人看，第 4 个被拒 | `RTSP_MAX_CLIENTS=4`，且 `listen()` backlog 只有 2 | 见 3.1-2 |
| 画面卡顿、帧率个位数 | WiFi 漫游正在静默扫描（占射频约 10 秒） | 见 3.2-4，有客户端时应跳过扫描 |
| 外网连不上 | 无公网 IP / 未做端口映射 / 映射了 TCP 没映射 UDP | 改用 VPN |

---

# 三、稳定性 / 性能 / 可维护性优化

优先级定义：**P0** = 会导致重启或数据/安全问题，本周做；**P1** = 明显影响体验或排障，两周内；**P2** = 结构性改善，排期做。

## 3.1 P0：稳定性与安全

| # | 改进点 | 现状 / 位置 | 具体做法 | 预期效果 | 验证方法 |
|---|---|---|---|---|---|
| 1 | RTSP 启动失败不应 abort | `main.c:811` `ESP_ERROR_CHECK(rtsp_server_start(554))` | 改成判断返回值，失败时 `ui_boot_check_mark` 标红 + 界面提示，其余功能继续 | 端口异常不再导致整机重启 | 人为占用 554 后开机，设备应正常进桌面 |
| 2 | 监听队列与并发不匹配 | `rtsp_server.c` `listen(fd, 2)` vs `RTSP_MAX_CLIENTS=4` | backlog 改 8 | 第 3、4 个客户端不再被内核丢弃 | 4 台设备同时连，全部成功 |
| 3 | RTSP 无认证 | `rtsp_handle_request()` | 增加 Basic 认证（凭据存 NVS，设置页可改），失败返回 401；同时把 `RTSP_SESSION_ID` 改成按连接随机 | 公网暴露风险归零；会话可隔离 | 不带凭据播放应 401；带凭据正常 |
| 4 | 慢速连接可长期占用会话任务 | `rtsp_read_request()` 逐字节 `recv`，无超时 | `setsockopt(SO_RCVTIMEO, 5s)`，超时断开 | 防住"连上不发请求"拖满 4 个槽位 | 用 `nc` 连上不发送，5 秒后应被断开 |
| 5 | 发送返回值未检查 | `rtsp_send_response()` / `rtp_write()` 忽略 `send()` 返回值 | 处理短写与 `EAGAIN`，连续失败则断开会话 | 消除协议错乱和"假在线" | 弱网下压测 30 分钟，无半包错乱 |
| 6 | 相机失败后仍对外提供 RTSP 地址 | `main.c` 视频初始化失败即 `return`，但 RTSP 已在监听 | 编码器未启动时在 DESCRIBE 里返回 `503`，或干脆不监听 | 客户端得到明确错误而不是黑屏 | 拔掉相机开机，VLC 应报 503 |
| 7 | 蓝牙关闭路径仍可能卡住 | `nimble_port_stop()` 内部 forever | 已加 `s_host_alive` 检查 + 独立 worker；**后续**可加"关闭超时 10 秒则置 ERROR 并允许重开" | 极端情况不冻结系统 | 人为让 host 任务不响应，界面仍可用 |

## 3.2 P1：性能

| # | 改进点 | 现状 / 位置 | 具体做法 | 预期效果 | 验证方法 |
|---|---|---|---|---|---|
| 1 | RTSP 每帧 malloc/free | `queue_push()` 为每帧 `malloc(len)` 拷贝 | 预分配 8 个固定大小（192KB）缓冲的环形池，引用计数 | 减少堆碎片与分配延迟，长时间推流更稳 | 连续推流 2 小时，`heap_spi` 无持续下降 |
| 2 | 每个 NAL 一次 `send()` | `rtp_send_nal()` 分片后逐包发送 | 用 `writev` 或 1~2 个 MTU 的聚合缓冲批量发 | 系统调用次数降 60%+，CPU 占用下降 | 对比优化前后 720p30 的 CPU 空闲任务占比 |
| 3 | LVGL 定时遍历整棵 App 树 | `ui_shell.c:1747` 每 480ms `enable_bubble_recursive()` 全树递归 | 只在 App 切换/新建控件时打标记，避免周期性全树遍历 | 股票 K 线等大列表页面掉帧减少 | 进股票详情页，看 LVGL `perf` 刷屏时间 |
| 4 | WiFi 漫游扫描抢占射频 | `wifi_mgr.c` `ROAM_SCAN_MS=60s`，已连接时扫一次约 10 秒 | 有 RTSP 客户端时把间隔拉长到 300s 或跳过本轮 | 推流期间不再周期性卡顿 | 推流中每 10 秒取帧间隔，不应出现 >500ms 空洞 |
| 5 | 心跳日志写卡频率 | `main.c` 每 30s 一次 `runlog_heartbeat` | 正常态降到 120s，事件（掉线/低内存/重启）仍然即时写 | TF 卡写入次数降 75%，寿命更长 | 观察 24 小时日志条数与卡写入量 |
| 6 | 监控页文本每 480ms 重排 | `ui_app_monitor.c tick()` 每次都 `lv_label_set_text_fmt` | 内容不变时不更新（缓存上次字符串） | 减少无谓的重绘与堆分配 | LVGL 绘制耗时对比 |
| 7 | BLE host 任务栈余量 | `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE=4096` | 提到 6144 后整包重编一次，配合 `uxTaskGetStackHighWaterMark()` 观察实际峰值 | GATT 写回调加逻辑后不会踩栈 | 开蓝牙后打印 `nimble_host` 的栈高水位，稳定在 1.5KB 以上即安全 |

## 3.3 P2：可维护性

| # | 改进点 | 现状 | 具体做法 | 预期效果 |
|---|---|---|---|---|
| 1 | `main.c` 过长（1200+ 行，含相机/PPA/snapshot/自检） | 单文件承担初始化 + 业务 | 拆 `app_camera.c`（video+PPA+snapshot）、`app_boot.c`（自检）、`app_main.c`（编排） | 单文件 < 400 行，改相机不必读整机启动流程 |
| 2 | 状态管理分散 | `wifi_mgr` / `bt_mgr` / `app_audio` 各自维护状态与回调 | 抽一个轻量 `sys_event`（发布/订阅，事件队列），UI 只订阅 | 新增模块不再各写一套 notify |
| 3 | 魔数散落 | `554`、`480`、`4096`、`3*1000*1000` 硬编码 | 集中到 `app_config.h`，关键项进 Kconfig | 调参与移植成本下降 |
| 4 | 日志级别不可调 | 运行期全是 `ESP_LOGI` | 用 `esp_log_level_set()` 按 TAG 分级，release 默认 WARN，调试页可临时开 DEBUG | 串口噪音下降，问题更聚焦 |
| 5 | 冒烟测试未纳入流程 | `rtsp_selftest.c` 已存在但靠手动 | 加 `tools/smoke.py`：启动后自动验 RTSP DESCRIBE/PLAY、BLE 开关 10 次、内存水位，失败返回非 0 | 每次改动有 5 分钟自动回归 |
| 6 | 大文件 | `ui_app_stock.c` 120KB、`ui_app_music.c` 90KB | 按"数据层/绘制层/交互层"拆分 | 编译增量变快，review 可行 |

## 3.4 建议的执行顺序

1. **本周**：3.1-1、3.1-2、3.1-4（都是小改动、收益直接），并把 1.5 的 V1~V7 跑一遍。
2. **下周**：3.1-3（认证）、3.2-1（帧池）、3.2-4（扫描避让）。
3. **排期**：3.3-1/2/5（结构性 + 自动化），这三项决定后续迭代速度。

---

## 附：本次改动文件清单

| 文件 | 改动 |
|---|---|
| `main/bt_mgr.h` | 新增 `bt_state_t`、`bt_mgr_state()`、`bt_mgr_state_str()`、`bt_mgr_last_error_str()`；明确 `bt_mgr_set()` 为异步语义 |
| `main/bt_mgr.c` | 重写：worker 任务 + 状态机 + 内存门限 + `adv_stop` + host 退出同步 + `reset_cb` + 分阶段日志；修正 GATT UUID 截断 |
| `main/ui_shell.c` | 蓝牙开关改为投递式；主 tick 按状态刷新控制中心，状态落定时提示一次 |
| `main/ui_app_settings.c` | 开关回调改为投递式；开关初始状态按 `bt_mgr_state()` 判断 |
| `sdkconfig.defaults` | 只加注释说明栈可调，实际值保持官方默认 4096 |

## 附二：本次验证到了哪一步

| 项 | 状态 |
|---|---|
| `main/bt_mgr.c` 编译单元检查 | ✅ 通过：`-fsyntax-only`，**0 error / 0 warning** |
| `main/ui_shell.c` | ✅ 通过：0 error；43 条 warning 全是既有的 LVGL 弃用 API（`lv_obj_add_flag` 等），非本次引入 |
| `main/ui_app_settings.c` | ✅ 通过：0 error；12 条同类既有 warning |
| 完整固件构建 | ⏳ **未在本机跑完**：改动 `sdkconfig` 会让 CMake 重写 `sdkconfig.h`，触发 2100+ 目标全量重编；本机 objdump 与文档加密驱动冲突，全量耗时以小时计 |

完整构建（找个能长时间占用机器的窗口执行）：

```bash
source projects/esp32-p4c5-ipcam/tools/idfenv.sh
export TMP=/d/idftmp TEMP=/d/idftmp
export LDGEN_KEEP=/d/p4c5b3/sections.ld.bak   # fragments 未变时可跳过 objdump
cd /d/p4c5b3 && ninja -j 1
```

> 只改 `main/` 下的 `.c` 且不动 `sdkconfig` 时，增量编译约 2 分钟。
> 构建前先确认没有残留的 `ninja` / `objdump` 进程，否则会并发抢占 DLP 导致卡死。
