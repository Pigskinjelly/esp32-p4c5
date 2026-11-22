# 复审报告（2026-11-21）整改记录 —— V2.39

- 整改日期：2026-11-22
- 基线：报告 `docs/ESP32-P4C5_复审报告_2026-11-21.md`
- 固件：`PROJECT_VER = 2.39`（bin 内 `App version: 2.39`）

## 一、逐条处理

| 编号 | 报告问题 | 处理 | 位置 |
|---|---|---|---|
| R-31 | README 快速开始路径错、环境变量未说明 | 路径改为 `projects/esp32-p4c5-ipcam`；构建段重写为「只用 `bash build.sh`」，并列出 `BUILD_DIR / IDF_PATH / IDF_PY_ENV / TMPDIR_IDF / ESP_IDF_VERSION` 五个变量的默认值与作用；补 `CMakeCache ... does not match the source` 的处理办法（`bash build.sh clean`） | `projects/esp32-p4c5-ipcam/README.md` |
| R-32 | SDIO 根因补丁未进清单、无 patch 文件 | ①`REPLICATION.md` §3 已有该行（本仓库前一轮已补）；②新增 `docs/patches/esp_hosted_sdio_rx.patch`（可直接 `git apply`）；③根因文档已在 `docs/R-30-SDIO接收缓冲.md` | `docs/patches/` |
| R-32b | `RX_NONE` 禁用约束只写在交接文档 | 已在 `REPLICATION.md` §3 末尾的「配置约束」写明；`DEVELOPMENT.md` R-30 段也写了「另需避免开启」，本轮确认无需再改 | `docs/REPLICATION.md` |
| R-33 | App 开关位图随注册顺序错位 | `app_off_load()` 增加 `app_off_ver` 迁移：旧 `bit5→4 / bit6→5 / bit7→6`，`bit4`（导航）丢弃；`app_off` 与 `app_bg` 同时迁移并回写 | `main/ui_shell.c` |
| R-34 | 版本串停在 `2.34-audit` | `PROJECT_VER = "2.39"`，bin 已确认 | `CMakeLists.txt` |
| R-35 | 「开 backtrace 就能定位 R-01」不成立 | `DEVELOPMENT.md` 补「R-01 诊断方法」小节（bootloader log level + PSRAM/flash 降频 A/B + 供电记录），并更正 `开发交接文档` 里那句错误预期 | `docs/DEVELOPMENT.md` 等 |
| R-36 | 行情任务创建失败无提示 | 交易 App：创建失败先降栈重试一次（8192→6144），仍失败则删除信号量、打错误日志并 toast「内存不足：行情暂不可用」，不再留半死状态；天气任务：同样检查返回值，失败即释放信号量并报错 | `main/ui_app_stock.c`、`main/ui_app_weather.c` |
| R-37 | 探针文件残留 / 调试函数无条件编译 / 注释错 | 删除根目录 `_t2~_t7.{py,log}`、`_db*.txt`、`_magic.txt`；**核查结论：8 个 `ui_stock_debug_*` 已在 `#if STK_DEBUG_API 0` 内，map/bin 中 0 符号，报告此条不成立**；`ui_shell.h` 的「4=导航」改为「4=音乐」 | 仓库根、`main/ui_shell.h` |

## 二、未处理（需要授权 / 属于产品决策）

| 编号 | 原因 |
|---|---|
| R-01 | 需真机 20 次冷启动取证（本轮 COM19 无任何输出，设备疑似断电/开关不在 P4 位，无法烧写与采样） |
| R-06 / R-10 | coredump 需要改分区表（会全片擦除、NVS 丢失），需你确认开窗 |
| R-07 | RTSP 鉴权需先定口令策略（固定 / 随机 / 是否与设备屏交互） |
| R-17 | 音频 sink 双重创建竞态，属 P3；改动需要改播放链路状态机，建议单独一轮并配真机听感验证 |

## 三、验证方式

- 静态：全量编译通过（0 error）；`grep` 确认 `ui_stock_debug_*` 不在 map/bin；
  `grep -a "2.39" esp32-p4c5-demo.bin` 命中。
- 运行：本轮烧写失败（COM19 可打开但 **0 字节**，手动 DTR/RTS 复位也无声，esptool 连接超时）
  —— 板子疑似断电或 P4/C5 拨码开关不在 P4 挡。待硬件恢复后补：
  冷启动 → 看 `reset reason`、`country code CN`、`band mode: 2.4G + 5G`、
  目标 SSID 不在列表时的 `NOT in scan result` + 可见 AP 清单 → 打开天气 App 看背景与排版。

## 四、天气 App 改动（同批交付）

| 项 | 说明 |
|---|---|
| 动态背景 | 480×480 PSRAM RGB565；时段（夜/黎明/上午/白天/黄昏/入夜）取三色做竖向渐变，再按天气码（晴/多云/阴/雾/雨/雪/雷）混色调；只在「时段 × 天气档」变化时重绘 |
| 排版 | 城市+定位点 / 更新时间 / 96px 大温度 + 圆环度号 / 天气·体感·AQI / 5 格逐小时（首格「现在」高亮卡）/ 未来 3 天温度区间条（蓝橙渐变 + 低温高温圆点）/ 底部湿度·风速·星期 |
| 图标 | 晴（太阳+4 道光芒）、云、阴、雾、雨、雪、雷，全部用 LVGL 图元拼，不依赖图片资源 |
| 数据 | 主接口加 `hourly=temperature_2m,weather_code&forecast_hours=6`；另请求 `air-quality-api` 的 `us_aqi` 显示「AQI 32 优」（失败保持 --，刷新沿用上次值） |
| 兼容 | 96px 数字字体不含 `-`：负温度单独画小横杠并把数字右移；城市名点=下一个、长按=上一个 |

