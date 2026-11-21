# ESP32-P4C5 整机 Demo（相机 + 屏 + 音频）

基于 OSPTEK 鱼鹰光电 `devboard-esp32p4c5_module` 开发板（ESP32-P4C5 核心板，硬件版本 **V1.5**）的
「相机实时预览到屏 + 麦克风→喇叭音频回环」整机 demo。

## 硬件清单

| 部件 | 型号 | 接口 |
|---|---|---|
| 开发板 | ESP32-P4C5-Module（V1.5） | — |
| 显示屏 | 4.3″ 480×800 MIPI（ST7102，含电容触摸）YDP430BT009-V1 | J3（30P 0.5mm） |
| 摄像头 | SC2336（2MP，24P 0.5mm） | J1（24P 0.5mm） |
| 喇叭 | 2415（8Ω/1W，2P-1.25 端子） | J9（MX1.25-2P） |

> 音频编解码 ES8311、功放 NS4150B、麦克风 LMA3729 均在板上，无需额外购买。

## 关键引脚映射（V1.5 底板，已核对原理图）

| 功能 | 引脚 |
|---|---|
| 共享 I2C（相机/触摸/ES8311） | GPIO7=SDA、GPIO8=SCL |
| MIPI-DSI | 2-lane（模块 34–39 脚） |
| LCD 复位 / TE | GPIO5 / GPIO4（本 demo 未用复位脚） |
| 相机 CSI | 2-lane（模块 41–46 脚）；MCLK=板上 24MHz 晶振 |
| 相机复位 CSI_RST | GPIO26（1.8V 域，需开漏；本 demo 默认关闭） |
| 音频 I2S0 | GPIO9=DOUT、GPIO11=DIN、GPIO10=WS、GPIO12=BCLK、GPIO13=MCLK |
| 功放使能 | GPIO53（NS4150B STD，高有效） |
| 调试串口 UART0 | GPIO37=TX、GPIO38=RX（CH343P → Type-C J7） |

## 环境

- ESP-IDF **v5.5.5**（本机 `D:\Espressif\frameworks\esp-idf-v5.5.5`）
- 目标芯片 `esp32p4`，Flash 16MB（QIO），PSRAM 32MB（200MHz）
- 依赖组件（自动下载）：`esp_video`（0.8.x，含 SC2336 传感器驱动）、`esp_codec_dev`、`es8311`

## 编译

```powershell
# 用 ESP-IDF PowerShell 终端，或：
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$env:IDF_PYTHON_ENV_PATH = "D:\Espressif\python_env\idf5.5_py3.11_env"
. "D:\Espressif\frameworks\esp-idf-v5.5.5\export.ps1"
cd <用户目录>\Desktop\ESP32-P4C5\esp32-p4c5-demo
idf.py build
```

## 烧录

1. 板上拨动开关拨到 **P4**。
2. USB 线接调试口 Type-C（J7，CH343P）。
3. 烧录并看日志：
   ```powershell
   idf.py -p COMx flash monitor
   ```
   （`COMx` 在设备管理器里查 CH343 串口号）

## 预期现象

- 串口打印：共享 I2C 创建成功 → LCD 初始化 → ES8311 初始化 → 相机打开 → 视频流开始 → 音频回环任务启动。
- 屏幕显示相机实时画面（480×800），串口周期性打印 `fps: ...`。
- 对着板上麦克风说话，喇叭应能听到声音（功放已使能）。

## 目录结构

```
esp32-p4c5-demo/
├── main/
│   ├── main.c          # 整合：共享I2C → LCD + 相机 + 音频
│   ├── app_lcd.c/h     # ST7102 屏（厂商驱动组件 esp_lcd_st7102）
│   ├── app_video.c/h   # SC2336 相机（esp_video 框架）
│   ├── app_audio.c/h   # ES8311 音频回环（esp_codec_dev）
│   └── Kconfig.projbuild
├── components/esp_lcd_st7102/   # 厂商 LCD 驱动（原样）
├── sdkconfig.defaults
├── partitions.csv               # nvs + factory(8M) + storage(7M)
└── CMakeLists.txt
```

## 已知注意事项

1. **音频**：`app_audio.c` 使用 `esp_codec_dev` + `es8311` 组件。首次编译若遇到组件 API 不一致，
   会在这里体现，需按实际组件版本微调（回环逻辑不变）。
2. **相机复位脚 GPIO26**：为 1.8V 域且要求开漏输出，本 demo 沿用厂商做法**不主动复位**（依赖传感器上电复位）。
   若需软复位，须把 `CONFIG_EXAMPLE_MIPI_CSI_CAM_SENSOR_RESET_PIN` 设为 26 并配置为开漏，且确保不灌 3.3V。
3. **屏复位脚 GPIO5**：厂商屏驱动 `app_lcd.c` 里 `EXAMPLE_PIN_NUM_LCD_RST = -1`（未用），屏依赖自身上电时序。
4. **Wi-Fi/BLE**（可选）：由核心板 C5 通过 SDIO 提供，需先给 C5 烧 ESP-Hosted 固件
   （`firmware/ESP32-C5_ESP-Hosted_v2.12.9_merged_0x0.bin`，烧到 C5 的 0x0），本 demo 不含无线。

## 参考资料

- 开发板仓库：https://gitee.com/osptek/devboard-esp32p4c5_module
- 屏驱动示例：https://gitee.com/osptek/display-tft-4.3inch-480x800
- 核心板仓库：https://gitee.com/osptek/esp32-p4c5-core-board
