# ESP32-P4C5 网络监控设备（ipcam）

SC2336 摄像头 → ISP → H.264 硬编 → RTSP 推流，同时 480×480 屏本地预览。

## 硬件

| 部件 | 型号 | 接口 |
|---|---|---|
| 开发板 | OSPTEK ESP32-P4C5-Module **V1.5** | — |
| 屏幕 | OSPTEK **YDP395BT008-V1** 3.95″ 480×480 | MIPI-DSI 2-lane，ST7102 |
| 摄像头 | **SC2336** 2MP | MIPI-CSI 2-lane，24MHz 板载晶振 |
| 音频 | ES8311 + NS4150B + 板载硅麦 | I2S0 |

## 引脚（V1.5 底板）

| 功能 | GPIO |
|---|---|
| 共享 I2C（相机/触摸/ES8311） | SDA=7，SCL=8 |
| LCD 复位 / TE / 背光 | 5 / 4 / 20 |
| 触摸 复位 / 中断 | 6 / 21 |
| 相机复位 CSI_RST（1.8V 域，开漏） | 26 |
| I2S：MCLK/BCLK/WS/DOUT/DIN | 13 / 12 / 10 / 9 / 11 |
| 功放使能（高有效） | 53 |
| 调试串口 UART0 | TX=37，RX=38 |
| SDIO→C5（由 esp_hosted 占用，勿动） | 14–19 |
| C5 复位 | 54 |

## 链路

```
SC2336 **1280x720** RAW10（默认；设置里可切 800×800，见 R-25）
   └─ ISP → YUV420
        ├─ PPA（缩放 + YUV→RGB565）→ ST7102 480x480 屏
        └─ /dev/video11 H.264（USERPTR 零拷贝）→ RTP → RTSP :554/stream
```

## 目录

```
main/
├── main.c            启动编排 + 帧回调（预览 + 编码）
├── app_lcd.c/h       ST7102 480x480 驱动（含复位时序坑处理）
├── app_video.c/h     SC2336 采集（esp_video）
├── app_audio.c/h     ES8311 音频
├── app_encoder.c/h   H.264 M2M 编码（新增）
└── rtsp_server.c/h   RTSP/RTP 服务器（新增）
components/esp_lcd_st7102/   厂商屏驱动
```

## 编译

本机装有 Ocular 文档加密（`TFsdframe.sys`），会加密 CMake/ninja 子进程写出的文件，
导致汇编器读到密文而报 `junk at end of line, first unrecognized character is '%'`。
经实测，**进程树的根决定是否被加密**：由 `bash.exe` 或 `cmd.exe` 启动的 gcc/ar 产物是明文，
由 `powershell.exe` 启动的会被加密。因此构建必须从 bash 发起。

### 首次构建（bash，一条命令）

```bash
cd <用户目录>/Desktop/ESP32-P4C5/projects/esp32-p4c5-ipcam
bash build.sh cfg     # 配置（首次、或改了 sdkconfig 之后跑一次）
bash build.sh         # 增量构建
bash build.sh clean   # 清掉构建目录重来
bash flash.sh COM19   # 烧写
```

`build.sh` 已经把下面这些环境变量都设好了，正常不需要手工 export：

| 变量 | 默认值 | 作用 / 为什么要设 |
|---|---|---|
| `BUILD_DIR` | `/d/p4c5b2` | 构建目录；只在想并行保留多份构建时才需要改 |
| `IDF_PATH` | `/d/Espressif/frameworks/esp-idf-v5.5.5` | ESP-IDF 安装路径，按本机实际改 |
| `IDF_PY_ENV` | `/d/Espressif/python_env/idf5.5_py3.11_env` | IDF 自带 Python 虚拟环境（本机 python3 是 3.13，不能用） |
| `TMPDIR_IDF` | `/d/idftmp` | 编译临时目录（放在文档加密驱动的监控范围外） |
| `ESP_IDF_VERSION` | `5.5` | **必须是 `5.5`**，不能写 5.5.5（原因见补丁表） |

> 换机器时：**只用 `bash build.sh`，不要 `idf.py build`，也不要在 Windows 终端里调 cmake**
> —— 那类进程树产出的目标文件会被文档加密驱动污染，gcc 报 `junk at end of line`。
>
> 若构建目录是从旧版本沿用的（源码路径变过），会报
> `CMakeCache ... does not match the source ...`，执行 `bash build.sh clean` 即可。

### 本机对 ESP-IDF 的三处补丁（已应用）

| 文件 | 改动 |
|---|---|
| `tools/partition_table/gen_esp32part.py` | `main()` 开头在 `sys.stdout is None` 时兜底到 devnull |
| `<cmake>/Modules/Internal/CheckSourceCompiles.cmake` | 环境变量 `IDF_SKIP_TRY_COMPILE` 存在时直接判定检查通过 |
| `<cmake>/Modules/CMakeDetermineCompilerABI.cmake`<br>`CMakeTestCCompiler.cmake` / `CMakeTestCXXCompiler.cmake` | 同上，跳过 try_compile |

这些补丁只在设置 `IDF_SKIP_TRY_COMPILE=1` 时生效，不影响其他项目。

## 烧录

1. 拨动开关拨到 **P4**；
2. USB 接调试口 Type-C（CH343P，本机为 COM19）；
3. `idf.py -p COM19 flash monitor`。

## 验证

| 阶段 | 预期现象 |
|---|---|
| 上电 | 串口打印 `LCD ready: 480x480`，屏幕亮并显示相机画面 |
| 采集 | 每 50 帧打印 `fps: ~30`，`camera format: 1280x720` |
| 编码 | `enc: N frames, last XXXXX B`，I 帧约 20–60KB |
| 推流 | VLC 打开 `rtsp://<设备IP>:554/stream`，5 秒内出画 |

VLC 建议：`vlc rtsp://192.168.x.x:554/stream --rtsp-tcp`（先试默认 UDP，不通再用 TCP）。

**没有第二台设备时**，可在 menuconfig 里打开 `IP Camera Network -> RTSP 回环自测`，
板子会连自己的 `127.0.0.1:554` 走完 OPTIONS/DESCRIBE/SETUP/PLAY 并统计 3 秒 RTP 包数。
实测参考值：406 包 / 534 KB / 3 秒 ≈ 1.4 Mbps（1280×720@30fps）。
自测会占用唯一的客户端名额，验证完记得关掉。

实测记录（2026-11-12）：

| 项目 | 结果 |
|---|---|
| Wi-Fi | 连接 `H***O`，DHCP 拿到 `<设备IP2>` |
| 本地预览 | 480×480 满屏（PPA 边界 0..479） |
| 采集/编码 | 30.0 fps 稳定，单帧 4–6 KB |
| RTSP 出流 | 回环自测 3 秒 406 包 / 534 KB |

## 已知限制

- RTSP 目前只服务单客户端；多客户端需扩展会话管理。
- 运动检测、mDNS、Web 面板尚未实现（P1/P2）。
- SPS/PPS 依赖首帧编码；若 DESCRIBE 早于首帧，SDP 会自动省略 `sprop-parameter-sets`，
  由码流内参数集兜底。

## 踩坑记录（已修，勿回退）

| 现象 | 根因 | 处理 |
|---|---|---|
| 屏幕画面偏左上、右/下黑边 | **PPA 缩放精度只有 1/16**（`SCAL_X_FRAG` 4 位）。`480/800=0.6` 被截断成 `9/16=0.5625`，实际只写 450×450 | 中心裁 768×768，按 `10/16=0.625` 缩放，输出正好 480（实测 bbox 0..479） |
| 连不上 Wi-Fi，日志 `reason 201` | 用的是占位 SSID `myssid`，附近无此网络 | 开机先扫描并打印周围热点，再连 `CONFIG_IPCAM_WIFI_SSID` |
| DSI 初始化直接 abort | `MIPI_DSI_PHY_CLK_SRC_DEFAULT` 是 rev<3.0 别名（PLL_F20M），rev3 上会走 HAL 的 `default: abort()` | 改用 `MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT`（XTAL） |
| bootloader 拒绝启动（revision 不匹配） | 工程继承了 rev<3.0 的 sdkconfig | `CONFIG_ESP32P4_REV_MIN_301=y`；分区表偏移同时改 `0x9000`（rev3 bootloader 24.7KB） |
| `esp_codec_dev_new()` 返回 NULL | 未设 `dev_type`，默认 0(NONE) | `.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT` |
| lwip `Invalid mbox` assert | RTSP 先于 `esp_netif_init()` | 先初始化 netif + 默认事件循环再起 RTSP |
| `CONFIG_WIFI_RMT_* undeclared` | `ESP_IDF_VERSION` 传成 5.5.5，`orsource ./idf_v$ESP_IDF_VERSION/` 找不到目录 | 构建时 `ESP_IDF_VERSION=5.5` |

