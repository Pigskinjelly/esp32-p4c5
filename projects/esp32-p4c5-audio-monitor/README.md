# ESP32-P4C5 远程音频监控（无屏/相机/喇叭，仅开发板）

用开发板自带的 **麦克风(LMA3729) + ES8311 编解码 + C5 双频 Wi-Fi** 实现「远程音频监听」：
板载麦克风采集 → 通过 Wi-Fi 推流 → 手机/电脑浏览器实时听到声音。

> 这是现有硬件（无相机）下唯一可行的「监控」形态；以后补一颗 SC2336 相机即可原地升级为视频监控。

## 硬件链路

```
麦克风 LMA3729 → ES8311 ADC → I2S0(GPIO9/10/11/12/13) → P4
                                                          │
P4 ── SDIO(GPIO14-19) ── C5(ESP32-C5, 跑 ESP-Hosted 固件) ── Wi-Fi ── 浏览器
```

| 部件 | 说明 |
|---|---|
| 无线 | ESP32-C5 通过 SDIO 连 P4，跑 ESP-Hosted-MCU 固件（已预编译） |
| 采集 | 板载模拟麦克风 LMA3729 + ES8311（I2C 0x18） |
| 推流 | WebSocket 广播 PCM(16kHz/16bit/单声道)，浏览器 Web Audio 播放 |

## 一、烧录 C5 无线固件（只需做一次）

1. 开发板上的拨动开关**拨到 C5**。
2. USB 线接调试口 Type-C（J7）。
3. 用 esptool 烧录预编译的 ESP-Hosted 固件到 `0x0`（固件在 devboard 仓库 `firmware/` 下）：

```powershell
esptool.py --chip esp32c5 -p COMx -b 460800 write_flash 0x0 ESP32-C5_ESP-Hosted_v2.12.9_merged_0x0.bin
```

4. 烧完把开关**拨回 P4**。

> 固件获取：https://gitee.com/osptek/devboard-esp32p4c5_module （firmware/ 目录）

## 二、编译并烧录 P4 程序

1. 先修一个已知兼容问题：`esp_hosted 2.12.13` 的 Kconfig 用了 IDF 6.1 才有的宏，
   在 IDF 5.5.5 下会解析失败。首次 `idf.py build` 下载完组件后，编辑
   `managed_components/espressif__esp_hosted/Kconfig`，把
   `depends on $(ESP_IDF_VERSION) >= "6.1"` 改成 `depends on n`（该行只是 SPI-HD 1-bit 选项，与 SDIO 无关）。

2. 配置 Wi-Fi（SSID/密码）：

```powershell
idf.py menuconfig   # 进入 "Remote Audio Monitor Config" 填 SSID 和密码
```

3. 编译烧录（开关保持 P4）：

```powershell
idf.py build
idf.py -p COMx flash monitor
```

## 三、使用

1. 串口日志看到 `got ip: x.x.x.x` 后，用手机/电脑（同一局域网）浏览器打开 `http://x.x.x.x/`。
2. 点「开始监听」，即可听到板载麦克风采集的实时声音。

## 关键技术点（已核对）

- **SDIO 引脚**：P4 的 SDMMC SLOT1（走 GPIO 矩阵）GPIO14-19，与 TF 卡(SLOT0, GPIO39-44) 独立，不冲突。
- **C5 复位**：P4 GPIO54 控制 C5 CHIP_PU（active-high）。
- **esp_hosted 配置**：`CONFIG_ESP_HOSTED_CP_TARGET_ESP32C5=y`，不选"开发板预设"，
  显式设置 SDIO 引脚（见 sdkconfig.defaults）。
- **应用侧是标准 esp_wifi API**：esp_wifi_remote 组件透明转发，无需显式调 esp_hosted。

## 风险与待验证（无法在本环境编译，需上板验证）

1. `audio_capture.c` 使用 esp_codec_dev 1.6.x 新 API（`es8311_codec_new` 等），已对照组件头文件核对，
   但未经编译验证，首次编译如有报错请把日志发我。
2. SDIO 引脚（GPIO14-19）依据核心板官方连接图，若 Wi-Fi 连不上，优先检查这里是否与实板一致。
3. 首次上电 C5 复位时序、SDIO 时钟等由 esp_hosted 默认处理，异常时再调。

## 目录结构

```
esp32-p4c5-audio-monitor/
├── main/
│   ├── main.c            # 入口：I2C + 音频 + Wi-Fi + Web
│   ├── wifi_sta.c/h      # Wi-Fi STA（标准 esp_wifi API）
│   ├── audio_capture.c/h # ES8311 麦克风采集（esp_codec_dev）
│   ├── audio_stream.c/h  # HTTP + WebSocket 推流 + 前端页面
│   └── Kconfig.projbuild # SSID/密码/采样率配置
├── sdkconfig.defaults    # 含 esp_hosted SDIO 引脚配置
└── partitions.csv
```
