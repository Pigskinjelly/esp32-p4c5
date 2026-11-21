# ESP32-P4C5 网络监控设备 · 架构设计基线

- 版本：v0.1（第二阶段方案设计）
- 日期：2026-11-11
- 硬件：OSPTEK ESP32-P4C5-Module 开发板 **V1.5** + 核心板 88Pin
- 屏幕：OSPTEK **YDP395BT008-V1** 3.95″ 480×480 MIPI-DSI（ST7102）
- 摄像头：**SC2336** 2MP MIPI-CSI（24P 0.5mm）

> 本文所有硬件结论来自：底板原理图 V1.3、模组使用指南 V1.5（2026-09-10）、核心板使用指南
> 2026-01-06、核心板仓库 `c5-p4-connection.png`、摄像头/屏幕/喇叭规格书。
> 标注「待确认」的条目不得在代码中固化。

---

## 1. 硬件基线（已确认）

| 项目 | 结论 | 来源 |
|---|---|---|
| 主控 | ESP32-P4，2×HP RISC-V 400MHz + LP | 核心板 P4 |
| 无线 | ESP32-C5HF4，Wi-Fi6 2.4/5G + BLE5 + 802.15.4 | 核心板 README |
| 存储 | 16MB Flash（QIO）+ 32MB PSRAM @200MHz | 核心板 README |
| C5↔P4 | **SDIO 4-bit + IO**，核心板内部已连 | c5-p4-connection.png |
| SDIO 引脚 | P4：D0=GPIO14 D1=GPIO15 D2=GPIO16 D3=GPIO17 CLK=GPIO18 CMD=GPIO19 | 同上 |
| C5 侧引脚 | D0=GPIO8 D1=GPIO7 D2=GPIO14 D3=GPIO13 CLK=GPIO9 CMD=GPIO10 | 同上 |
| C5 复位 | C5CHIP_PU ← **P4 GPIO54** | 同上 |
| C5 附加 IO | C5 GPIO2 ↔ **P4 GPIO6** | 同上 |
| 以太网 | 板上无 PHY；可选 PoE 转接板 IP101GRI（RMII） | devboard README |

### 1.1 视频与显示

| 信号 | 引脚 | 说明 |
|---|---|---|
| CSI 差分 | D0/D1/CLK，模组 41–46 | 2-lane |
| CSI MCLK | 板载 24MHz 晶振 | P4 不输出 MCLK |
| CSI_RST | **GPIO26**（1.8V 域，开漏） | 禁止推挽输出 3.3V |
| CSI SCCB | GPIO7/8 经 TXS0102 转 1.8V | 与触摸/codec 共享 |
| DSI 差分 | D0/D1/CLK，模组 34–39 | 2-lane，620Mbps |
| LCD_RST | **GPIO5** | 0R 到屏 |
| LCD_TE | **GPIO4** | 可选 |
| 背光 EN | **GPIO20** → SY7200 EN | 高有效，10K 上拉 |
| 触摸 INT/RST | **GPIO21 / GPIO6** | 见风险 R1 |

### 1.2 音频

| 信号 | 引脚 | 方向 |
|---|---|---|
| MCLK | GPIO13 | P4 → ES8311 |
| BCLK | GPIO12 | P4 → ES8311 |
| WS/LRCK | GPIO10 | P4 → ES8311 |
| DOUT (DSDIN) | GPIO9 | P4 → ES8311 |
| DIN (ASDOUT) | GPIO11 | ES8311 → P4 |
| I2C | GPIO7/8，地址 0x18（CE 下拉） | 双向 |
| PA 使能 | **GPIO53**，10K 下拉，高有效 | P4 → NS4150B STD |
| 喇叭 | 8Ω / 1W（最大 1.2W），NS4150B 5V 供电 | BTL |

> NS4150B 在 5V/8Ω 下理论输出约 1.5W，超过喇叭额定 1W。软件必须限幅。

---

## 2. 软件架构

### 2.1 分层

```
┌──────────────────────────────────────────────────────────────┐
│ 应用层   rtsp_server / web_panel / mdns / motion_detect / ui  │
├──────────────────────────────────────────────────────────────┤
│ 服务层   stream_pipeline（帧分发） / snapshot / event_bus     │
├──────────────────────────────────────────────────────────────┤
│ 驱动层   esp_video(V4L2) / esp_lcd_st7102 / esp_codec_dev     │
│          esp_hosted + esp_wifi_remote                         │
├──────────────────────────────────────────────────────────────┤
│ 硬件层   CSI+ISP / DSI / I2S+ES8311 / SDIO→C5→Wi-Fi           │
└──────────────────────────────────────────────────────────────┘
```

### 2.2 数据流

```
SC2336 ──MIPI CSI──> ISP ──> /dev/video0 (YUV422)
                              │
              ┌───────────────┼────────────────┐
              │               │                │
         PPA 缩放        H.264 M2M        帧差检测
      480×480 RGB565    /dev/video11     (降采样 120×120)
              │               │                │
              ▼               ▼                ▼
        LCD 帧缓冲      NAL 环形队列      event_bus
        (双缓冲)             │                │
              │              ▼                ▼
              ▼         RTP 打包 ──> SDIO ──> C5 ──> Wi-Fi
          屏幕显示            │
                        RTSP 会话管理
```

**关键原则：编码路径零拷贝。** CSI 采集缓冲直接作为 M2M 编码的 INPUT buffer，
不在用户态复制帧；预览与编码共享同一帧，靠引用计数避免撕裂。

### 2.3 缓冲区预算（PSRAM 32MB）

| 用途 | 数量 | 单块 | 合计 |
|---|---|---|---|
| CSI 采集（YUV422 480×480） | 3 | 450KB | 1.35MB |
| LCD 帧缓冲（RGB565） | 2 | 450KB | 0.9MB |
| H.264 输出 NAL | 4 | 64KB | 256KB |
| RTSP 发送队列 | 8 | 32KB | 256KB |
| LVGL 绘制缓冲 | 2 | 480×40×2 | 75KB |
| 运动检测缩略图 | 2 | 120×120 | 28KB |
| 余量 | — | — | ~28MB |

### 2.4 目录结构（建议新建工程 `esp32-p4c5-ipcam`）

```
esp32-p4c5-ipcam/
├── main/
│   ├── main.c                 # 启动编排（见 §4）
│   ├── board_pins.h           # 所有引脚集中定义（唯一真源）
│   ├── app_video.c/.h         # CSI + ISP 采集（复用 demo）
│   ├── app_display.c/.h       # ST7102 + PPA 预览（改 480×480）
│   ├── app_audio.c/.h         # ES8311 + 提示音（复用 demo）
│   ├── app_encoder.c/.h       # /dev/video11 M2M 编码
│   ├── stream_pipeline.c/.h   # 帧分发/引用计数
│   ├── rtsp_server.c/.h       # RTSP/RTP/RTCP
│   ├── web_panel.c/.h         # HTTP 状态 + 抓拍
│   ├── mdns_service.c/.h      # _rtsp._tcp 广播
│   ├── motion_detect.c/.h     # 帧差
│   └── Kconfig.projbuild
├── components/
│   ├── esp_lcd_st7102/        # 厂商驱动（从 demo 复制）
│   └── rtsp/                  # 协议栈（可参考 esp32p4-uvc-video）
├── partitions.csv             # factory 4M + storage 11M
└── sdkconfig.defaults
```

### 2.5 任务与核分配

| 任务 | 核 | 优先级 | 栈 | 职责 |
|---|---|---|---|---|
| cam_task | 1 | 6 | 4KB | V4L2 dqbuf/qbuf，30fps 节拍 |
| enc_task | 0 | 6 | 6KB | M2M 编码，产出 NAL |
| rtsp_task | 0 | 5 | 6KB | RTP 打包 + send |
| ui_task | 1 | 4 | 6KB | LVGL 状态刷新（5Hz 足够） |
| motion_task | 1 | 3 | 3KB | 帧差，2Hz |
| audio_task | 1 | 4 | 4KB | 提示音播放（按需） |
| httpd / mdns | 0 | 3 | 6KB | 系统默认 |

> 编码与 RTSP 放 Core0（网络/协议），采集与显示放 Core1（实时性）。避免 UI 与采集抢核。

---

## 3. 关键流程时序

### 3.1 冷启动（目标 < 2.5s 出图，< 6s 可拉流）

```
t=0     NVS / 日志 / 事件循环
t=50    I2C 总线（GPIO7/8, 400kHz）
t=80    MIPI PHY LDO3 = 2.5V
t=100   GPIO20 背光 = 0（先关，避免白屏）
t=120   GPIO5 LCD_RST 复位脉冲 → DSI 初始化（620Mbps, 2lane）
t=400   LCD 显示黑场，背光 = 1
t=420   GPIO26 CSI_RST 开漏复位 → SCCB 探测 SC2336（0x30）
t=600   /dev/video0 打开，设 YUV422 480×480@30
t=700   /dev/video11 打开，设 H.264 CBR 2Mbps
t=720   ES8311 初始化，GPIO53 = 0（静音）
t=750   采集启动 → 屏幕出图  ← 验收点 A
t=760   GPIO54 = 1，释放 C5 复位
t=1500  SDIO 枚举完成，esp_hosted 就绪
t=2000  Wi-Fi STA 连接（DHCP）
t=3000  RTSP server 监听 554
t=3100  mDNS 广播 _rtsp._tcp  ← 验收点 B
```

**设计要点：WI-FI 放在出图之后。** 网络阻塞不能拖慢首帧显示。

### 3.2 异常恢复

| 现象 | 检测 | 动作 |
|---|---|---|
| CSI 无帧 3s | cam_task 超时 | 复位 GPIO26，重开 /dev/video0 |
| 编码器卡死 | enc_task 超时 | 关闭重开 /dev/video11 |
| Wi-Fi 断开 | 事件回调 | 指数退避重连，期间本地预览不受影响 |
| C5 无响应 | SDIO 超时 | GPIO54 拉低 100ms 复位 C5，重新初始化 |
| RTSP 客户端异常断开 | RTCP 超时 | 释放会话，保留最后 IDR 供新客户端快速出图 |

---

## 4. 资源分配

### 4.1 GPIO 总表

| GPIO | 功能 | 模式 | 备注 |
|---|---|---|---|
| 4 | LCD_TE | 输入 | 可选，可不用 |
| 5 | LCD_RST | 输出 | 上电复位脉冲 |
| 6 | TP_RST / C5_GPIO2 | 输出 | **与 C5 共用，见 R1** |
| 7 | I2C_SDA | OD | 共享 |
| 8 | I2C_SCL | OD | 共享 |
| 9 | I2S_DOUT | 输出 | → ES8311 DSDIN |
| 10 | I2S_WS | 输出 | |
| 11 | I2S_DIN | 输入 | ← ES8311 ASDOUT |
| 12 | I2S_BCLK | 输出 | |
| 13 | I2S_MCLK | 输出 | 2.048MHz（48k×...）以 codec 要求为准 |
| 14–19 | SDIO D0–D3/CLK/CMD | — | **由 esp_hosted 占用，应用层禁用** |
| 20 | LCD_BL_EN | 输出 | 高开 |
| 21 | TP_INT | 输入 | 中断 |
| 26 | CSI_RST | 开漏输出 | 1.8V 域 |
| 37/38 | UART0 TX/RX | — | 调试口 |
| 53 | PA_EN | 输出 | 高有效，默认低 |
| 54 | C5_CHIP_PU | 输出 | 高=释放复位 |

### 4.2 总线

| 总线 | 实例 | 设备 | 速率 |
|---|---|---|---|
| I2C0 | GPIO7/8 | SC2336(0x30)、ES8311(0x18)、触摸(待确认) | 400kHz |
| MIPI CSI | ISP | SC2336 2-lane | 按 sensor 时序 |
| MIPI DSI | DSI host | ST7102 2-lane | 620Mbps |
| I2S0 | — | ES8311 | 16bit/16kHz 单声道 |
| SDIO | SDMMC slot1 | C5 | 4-bit，时钟待实测 |

---

## 5. 测试计划

| 层级 | 用例 | 判据 |
|---|---|---|
| 单元 | NAL 分片/组包 | 单帧 > MTU 时切片正确，序号连续 |
| 单元 | 环形队列满 | 丢最旧帧，不阻塞生产者 |
| 板上 | 点屏 | 纯色/彩条无花屏，TE 同步无撕裂 |
| 板上 | 预览帧率 | 30fps ±2，连续 30min 不掉帧 |
| 板上 | VLC 拉流 | 5s 内出画，延迟 <500ms，音视频（若启用）同步 |
| 板上 | 双客户端 | 2 个 VLC 同时拉流，各自正常 |
| 板上 | 运动检测 | 挥手 1s 内边框变色 + 提示音 |
| 稳定性 | 24h 老化 | 无内存泄漏（heap 波动 <5%），无看门狗复位 |
| 稳定性 | Wi-Fi 断连恢复 | 拔路由器 30s 后恢复，预览不中断 |
| 功耗 | 空闲/预览/推流 | 记录三态电流，评估 PoE 供电余量 |

---

## 6. 已知风险

| 编号 | 风险 | 影响 | 处置 |
|---|---|---|---|
| R1 | **GPIO6 同时接 C5 GPIO2 与 TP_RST** | 触摸复位可能受 C5 干扰 | 确认底板 R8 是否贴装；C5 固件须将 GPIO2 置高阻 |
| R2 | 触摸 IC 型号未定（ST7102/ST7123） | 触摸驱动选错 | 上电扫 I2C 地址确认 |
| R3 | NS4150B 5V 输出 > 喇叭 1W | 喇叭过载 | 软件限幅 -3dB，音量上限 80% |
| R4 | esp_hosted 与 C5 固件版本须匹配 | SDIO 通信失败 | P4 端锁 2.12.x，与 C5 固件一致 |
| R5 | IDF 5.5.5 vs 示例 5.5.2/5.5.4 | API 差异 | 统一 5.5.5，锁定组件版本 |
| R6 | 480×480 初始化序列需匹配屏批次 | 花屏/不亮 | 以 OSPTEK bringup 工程为准，逐条比对 |

> 上表为立项阶段的硬件风险清单。开发推进后的运行期风险与闭环状态（SDIO 接收缓冲、PSRAM 栈写 NVS、RTSP 鉴权、NVS 容量、版本标识等）见 `docs/ESP32-P4C5_复审报告_2026-11-21.md` 与 `docs/DEVELOPMENT.md`。
