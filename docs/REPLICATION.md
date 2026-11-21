# 复刻指南：从零构建 ESP32-P4C5 项目

本文目标：让拿到同款硬件与一份干净 ESP-IDF 的人，能复现本仓库固件的构建。内容整理自 `ESP32-P4C5_开发交接文档.md` §1–§2（已脱敏版本）。**原开发机有一系列"只在本机磁盘、不受 git 管理"的补丁，这是复刻最大的坑，务必逐条处理。**

## 1. 硬件基线（不可动摇的约束）

| 项 | 结论 | 备注 |
|---|---|---|
| 开发板 | OSPTEK ESP32-P4C5-Module **V1.5**（底板原理图 V1.3） | V1.3/V1.5 底板 P4 侧引脚一致 |
| 核心板 | ESP32-P4 rev v3.2 + ESP32-C5HF4 rev v1.2，88Pin | rev3 是若干配置坑的根源 |
| 主控 | P4 双核 HP RISC-V 400MHz + LP 40MHz；16MB Flash QIO + 32MB PSRAM @200MHz | — |
| 无线 | C5：Wi-Fi6 双频 + BLE5 + 802.15.4，经 **SDIO 4-bit** 接 P4 | 板上无以太网 PHY |
| 屏幕 | **YDP395BT008-V1** 3.95″ **480×480** MIPI-DSI 2-lane（ST7102 驱动 / **ST7123 触摸**，fw 1.71.1.5，5 点） | ⚠️ 早期 demo README 里的 4.3″ 480×800 / YDP430BT009 是旧资料，勿据此改代码 |
| 摄像头 | SC2336 2MP MIPI-CSI 2-lane，24P 0.5mm，板载 24MHz 独立晶振 | P4 不输出 MCLK |
| 音频 | ES8311 codec + NS4150B 功放 + 板载 LMA3729 麦 | PA_EN=GPIO53；5V/8Ω 理论 1.5W > 喇叭 1W，软件须限幅 |
| 存储 | TF 卡 SDMMC SLOT0 + flash `storage` 分区 7M FATFS | 有卡优先用卡 |
| C5 固件 | `firmware/ESP32-C5_ESP-Hosted_v2.12.9_merged_0x0.bin`，烧到 C5 `0x0` | 与 P4 侧 esp_hosted 版本必须严格对齐 |

**P4 ↔ C5 SDIO 引脚（核心板内部已连，勿用官方板预设）**

| 信号 | P4 | C5 |
|---|---|---|
| D0 / D1 / D2 / D3 | 14 / 15 / 16 / 17 | 8 / 7 / 14 / 13 |
| CLK / CMD | 18 / 19 | 9 / 10 |
| C5 复位 | P4 GPIO54 → C5CHIP_PU | — |

> esp_hosted 的 `ESP_HOSTED_P4_C5_CORE_BOARD` 预设是乐鑫官方板（CMD=51 / CLK=50），与本板不符；必须手填 `CONFIG_ESP_HOSTED_PRIV_SDIO_PIN_*_SLOT_1`。

**Strapping**：P4 下载模式 = GPIO35 低 + GPIO36 高；SPI Boot = GPIO35 高（默认弱上拉）。C5 启动模式 GPIO26/27/28，打印控制 GPIO25。模块第 66 脚 = ESP_LDO_VO4（0.5–2.7V / 3.3V，200mA）。

完整 GPIO 总表见交接文档 §1.2、[`ARCHITECTURE.md`](ARCHITECTURE.md) §4 与各工程 `sdkconfig.defaults`。

## 2. 构建环境

- ESP-IDF **v5.5.5**，Python **3.11** venv，交叉编译器 riscv32-esp-elf，CMake 3.30.2，Ninja 1.12.1。
- 组件版本由各工程 `main/idf_component.yml` 与 `dependencies.lock` 锁定（`managed_components/` 已随仓库提供）。
- 关键环境变量（已固化在各工程 `build.sh`，**不要删除**）：

| 变量 | 值 | 原因 |
|---|---|---|
| `BUILD_DIR` | 非桌面路径（原机 `/d/p4c5b2`） | 避免构建目录落在被加密敏感的路径 |
| `TMP`/`TEMP` | 非 C 盘 Temp（原机 `/d/idftmp`） | 同上 |
| `IDF_SKIP_TRY_COMPILE` | `1` | 跳过 CMake try_compile（DLP 会锁临时目录报 "could not be removed"） |
| `CFLAGS`/`CXXFLAGS`/`ASMFLAGS` | `-pipe` | 不落临时 `.s` 文件 |
| `IDF_CCACHE_ENABLE=0` / `CCACHE_DISABLE=1` | — | ccache 删 depfile 会被 DLP 拒绝 |
| **`ESP_IDF_VERSION`** | **`5.5`**（不是 5.5.5） | esp_wifi_remote / esp_hosted 的 Kconfig 用 `./idf_v$ESP_IDF_VERSION/` 拼路径，传 5.5.5 会让 Wi-Fi 选项被 `orsource` 静默跳过 → `CONFIG_WIFI_RMT_* undeclared` |

### 2.1 最大的坑：文档透明加密驱动（DLP）

原开发机装了 TEC Ocular 3 加密驱动（`TFsdframe.sys` / `TsdEncrypt`），会加密部分子进程写出的文件，使 gcc 报 `junk at end of line, first unrecognized character is '%'`。**关键发现：加密与否取决于进程树的根** —— `powershell.exe → cmake/gcc/ar` 产物被加密；`bash.exe`/`cmd.exe → cmake/gcc/ar` 产物是明文。

**结论：所有构建一律从 bash 发起（`bash build.sh`），不要 `idf.py build`，不要在 PowerShell 里调 cmake。** 没有该驱动的机器不受影响，但仍建议沿用 bash 脚本以保持一致。

## 3. 必须重打的补丁（原机仅在本机磁盘，不在 git 中）

以下补丁整理自交接文档 §2.3。其中组件侧补丁已经随 `managed_components/` 入库，**IDF 工具链侧补丁（`ldgen.py` 等）不在仓库中，需要按下面说明重打**。仓库内另附 `docs/patches/README.md` 与提取出的 `ldgen.py` 片段供参考。

| 位置 | 改动 | 作用 / 触发条件 |
|---|---|---|
| `tools/ldgen/ldgen.py` | 新增 `_objdump_sections()`：objdump 经 `cmd.exe` 执行 + 校验 `In archive`/`Sections:` + **连续两次输出一致才接受** + 12 次重试 + `subprocess.run(timeout=20)` + 三种启动方式轮询 | 解决 DLP 导致 objdump 空输出/截断/**0 CPU 无限挂起**；无 DLP 的机器通常不需要，但保留更稳 |
| `tools/partition_table/gen_esp32part.py` | `main()` 开头 `sys.stdout is None` 时兜底到 devnull | 环境变量异常下不崩 |
| `Modules/Internal/CheckSourceCompiles.cmake`、`CMakeDetermineCompilerABI.cmake`、`CMakeTest{C,CXX}Compiler.cmake` | 有 `IDF_SKIP_TRY_COMPILE` 时直接判定检查通过 | 跳过 try_compile |
| `managed_components/espressif__esp_video/.../esp_video_jpeg_device.c` | `timeout_ms = 40 → 300` | 1080p JPEG 编码（**已随仓库提供**） |
| `managed_components/espressif__esp_wifi_remote/idf_v5.5/include/injected/esp_wifi_types_generic.h` | 补 `gtk_rekey_interval` 字段 | 否则 esp_hosted 编译失败（**已随仓库提供**） |
| `managed_components/espressif__esp_lcd_touch_st7123/.../esp_lcd_touch_st7123.c` | 触点按面积降序 + 数组清零 + max_touches 保护 | 多点误触修复（**已随仓库提供**） |
| `esp_lvgl_port_touch.c` | `ESP_ERROR_CHECK(read_data)` → 容错 | I2C 抖动不再 assert 重启 |
| `espressif__esp_hosted/Kconfig`（audio-monitor 工程） | `$(ESP_IDF_VERSION) >= "6.1"` → `depends on n` | IDF 5.5.5 无法解析 6.x 宏（audio-monitor 历史问题） |

> 标记"已随仓库提供"的补丁在 `managed_components/` 中已是修改后版本；`managed_components/` 由 ESP-IDF 组件管理器管理，重新 `idf.py reconfigure` 可能被覆盖，改动前先备份。

## 4. 构建故障速查

| 现象 | 处理 |
|---|---|
| `junk at end of line ... '%'` | 用了 PowerShell 链 → 改 `bash build.sh` |
| `libriscv.a: file format not recognized` | 静态库被污染 → 删除构建目录下所有 `*.a` 后 `bash build.sh` |
| `CONFIG_WIFI_RMT_* undeclared` | `ESP_IDF_VERSION` 必须设 `5.5` |
| ldgen 卡住 10 分钟 / `Sections:` 解析失败 | objdump 被加密驱动挂死 → 确认 ldgen.py 补丁的 `timeout=20` 与重试逻辑 |
| 某 gcc 进程 0 CPU 不动 | 杀掉该 gcc → ninja 报 FAILED → 重跑 `bash build.sh` |
| 改 `sdkconfig` 触发全量重编（约 10 分钟） | 预期行为，尽量批量改配置 |

## 5. 已知限制

- 1080p 实测不可用；校准用 640×640。
- 冷启动偶发一次启动失败后自动重启（bootloader 阶段 WDT，频率低，未根治）。
- 导航 App 已冻结归档于 `docs/frozen/map-app/`，桌面已注销。
- 交易/音乐等在线功能依赖第三方 HTTP 接口，可能随平台策略失效；网易云在线曲库需 OAuth + 会员，设备端未实现，采用免 Key 电台方案。
- 助手自测通过 ≠ 用户真机验收；多数历史轮次的逐项真机验收仍缺，详见交接文档。
