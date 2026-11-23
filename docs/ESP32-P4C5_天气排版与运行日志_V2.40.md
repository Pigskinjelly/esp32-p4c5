# 天气界面排版修复 + TF 卡运行日志（V2.40）

日期：2026-11-23
固件：2.39 → 待递进

---

## 一、天气界面排版

### 报出的三个问题与根因

| 现象 | 根因 |
|---|---|
| 底部文字超出屏幕 | 湿度/风速/日期写死 `y=462`，而 `ui_cjk_16` 行高 22 → 底边 484，超出 480 屏 4px，被裁掉 |
| 温度字重过粗 | 字体是用 `ariblk.ttf`（Arial Black，超黑体）生成的 `ui_font_time96`，天生粗 |
| 度数符号与温度重叠 | 度符号写死 `x = 12 + 118`，而 96px 字宽的两位数字实际宽度不固定；`"26"` 比 `"--"` 宽，`"-12"` 更宽，写死必然错位 |
| 右侧天气与温度间距过大 | 写死 `x = 214`，温度实际只到 ~118，中间空了近 100px |

### 改法

**1. 换细体字体**

用 `lv_font_conv` 以 `segoeuil.ttf`（Segoe UI Light）重新生成 96px 字体：

```bash
lv_font_conv --font C:/Windows/Fonts/segoeuil.ttf --size 96 --bpp 4 \
  --format lvgl --no-compress --range 0x2D-0x3A \
  --lv-font-name ui_font_time96_light -o ui_font_time_96_light.c
```

- `--range 0x2D-0x3A` 包含负号 `-`，所以**负温度不再需要单独画横杠**，直接由字体渲染；
- 生成后要手动把 `#include "lvgl/lvgl.h"` 改成 `#include "lvgl.h"`（本项目 LVGL 头路径不同，否则编译报 `lvgl/lvgl.h: No such file`）；
- 度量：`line_height = 80`（旧 Arial Black 是 72）。

**2. 度符号与右侧信息改为动态定位**

新增 `layout_hero()`，在温度文本定稿后测量实际宽度再摆放：

```c
lv_obj_update_layout(s_lbl_temp);
lv_coord_t tw = lv_obj_get_width(s_lbl_temp);

/* 度符号贴数字右侧 3px，垂直偏上 */
lv_obj_set_pos(s_deg, HERO_X + tw + 3, HERO_Y + 12);

/* 右侧三行：距温度右边界 30px；温度特别宽时收缩到安全线 */
lv_coord_t rx = HERO_X + tw + 30;
if (rx > 480 - 16 - 110) rx = 480 - 16 - 110;
```

`apply_ui()` 末尾统一调用一次（`"--"` 分支也要调，否则首帧错位）。

**3. 底部改贴底对齐**

```c
lv_obj_align(s_lbl_hum,  LV_ALIGN_BOTTOM_LEFT,  16, -12);
lv_obj_align(s_lbl_wind, LV_ALIGN_BOTTOM_MID,    0, -12);
lv_obj_align(s_lbl_date, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
```

不再依赖绝对 y，屏幕高度变化也不会溢出。

**4. 中部重排（腾出底部空间）**

| 区块 | 旧 | 新 |
|---|---|---|
| 逐小时高亮卡 | y=190, h=96 | y=168, h=92 |
| 逐小时文字/图标/温度 | 198 / 222 / 254 | 176 / 200 / 232 |
| 未来 3 天卡片 | y=300, h=158（绝对定位） | `ALIGN_BOTTOM_LEFT, 16, -50`，h=148 |
| 卡片内行距 | 40 + i*38 | 34 + i*36 |
| 底部三行 | y=462 | `ALIGN_BOTTOM` −12 |

垂直预算：温度 68+80=148 → 逐小时 168~260 → 卡片 282~430 → 底部 446~468 → 屏底 480。

### 验证

编译通过；需真机目视确认（细体观感、度号位置、底部是否贴边）。

---

## 二、TF 卡运行日志（新增 `runlog.c/h`）

### 为什么做

以前只能靠串口，拔线就什么都没了；偶发重启、卡顿、内存见底往往发生在没人看串口的时候。
把关键状态持续写进 TF 卡，事后读文件就能还原现场。

### 落盘位置与格式

```
/sdcard/LOGS/run-YYYYMMDD.jsonl      （无 TF 卡时回落 /storage/LOGS/）
/sdcard/LOGS/run-unsync.jsonl        （RTC 尚未对时，先写这里）
/sdcard/LOGS/run-YYYYMMDD-2.jsonl    （单文件超 4MB 滚动）
```

- **JSONL**：一行一条 JSON，UTF-8。既能 `cat` 直接看，也能 `jq` / Python 解析；
- 每天一个文件，保留 14 天；
- 跨天后自动切文件；对时成功（时间 > 2025-01-01）后从 `run-unsync` 切到当天文件。

一条记录：

```json
{"t":"2026-11-23 13:50:12.345","up":1234,"lvl":"I","mod":"sys","ev":"hb",
 "heap_int_kb":81,"heap_min_kb":4,"psram_kb":16350,"wifi":"已连接",
 "ip":"<设备IP>","rtsp_clients":0,"rtsp_pkts":0,
 "enc_frames":2447,"enc_keyframes":0}
```

字段：`t` 本地时间（未对时写 `-`）、`up` 上电秒数、`lvl` I/W/E、`mod` 模块、`ev` 事件，其余为附加键。

### 记录的事件

| ev | 内容 | 用途 |
|---|---|---|
| `boot/start` | 固件版本、复位原因（含 `rst_name`）、累计启动/异常次数、内部堆、PSRAM | 每次开机一行，能看出重启频率与原因 |
| `boot/crash` | 上次是 panic / 任务看门狗 / 中断看门狗 / 欠压 | 与 `start` 成对出现，异常重启一眼可见 |
| `sys/hb` | 内部堆(当前/最低)、PSRAM、WiFi 状态与 IP、RTSP 客户端/包数、编码帧数 | 30 秒一条，画趋势、找泄漏 |
| `sys/stall` | 日志任务被延迟 > 250ms + 当时堆余量 | **卡顿**：说明系统被长时间抢占/阻塞 |
| `sys/heap_low` | 内部堆 < 20KB（含最低水位） | 定位 SDIO/JPEG 分配失败的前兆 |
| `sys/heap_recover` | 堆恢复到 40KB 以上 | 与 `heap_low` 配对，看是否反复 |
| `wifi/state` | WiFi 状态变化 + IP | 事后知道什么时候断、断了多久 |
| `sys/log_paused` | 卡满暂停写 | 解释"为什么后面没有日志了" |

### 工程实现要点

1. **异步写**：`runlog_event()` 只做格式化 + `xQueueSend(..., 0)`（32 深、每条 320B），
   真正的 `fputs` 在 `rl_task`（优先级 1）里做。调用方（摄像头/UI/心跳）永不被 SD 卡阻塞。
2. **不反噬系统**：队列满就丢弃并计数（`drop=`），绝不用阻塞发送。
3. **掉电安全**：每 2 秒 `fflush`，最坏丢 2 秒；文件句柄常开，减少 open/close。
4. **卡满保护**：每 60 秒查剩余空间（`app_storage_usage`）。
   < 1MB 时从最旧文件开始删；仍 < 256KB 就暂停写并记 `log_paused`，
   避免日志把拍照/录像的空间挤掉。
5. **未对时处理**：RTC 是 1970 时文件名用 `run-unsync.jsonl`，否则所有日志都会塞进 `run-19700101`。
6. **`runlog_dump(n)`**：把当前文件最后 n 行打到串口。启动时调一次（12 行），
   不拔卡也能确认日志真的在写。

### 接入点（main.c）

```c
/* storage_task：卡挂载成功后 */
if (runlog_init()) {
    runlog_boot(s_boot_rst, s_boot_count, s_crash_count);
    runlog_dump(12);
}

/* heartbeat_task：每 30 秒 */
runlog_heartbeat(frames, keyframes, clients, pkts, wifi_mgr_state_str(...), wifi_mgr_get_ip());
```

心跳任务栈从 3072 提到 4096（日志格式化在栈上要 ~550B，原来太紧）。

### 验证（实测）

```
I (6510) runlog: runlog ready: /sdcard/LOGS/run-unsync.jsonl (TF, 72 KB free)
I (6517) runlog: ---- /sdcard/LOGS/run-unsync.jsonl (tail 12) ----
I (6520) {"t":"-","up":4,"lvl":"I","mod":"boot","ev":"start","ver":"2.39","rst":1,"rst_name":"poweron","boots":2,"crashes":0,"heap_int_kb":73,"psram_kb":16350}
I (6564) {"t":"-","up":4,"lvl":"E","mod":"boot","ev":"crash","rst":7,"rst_name":"other_wdt","note":"见串口 Backtrace"}
I (36465) wrote hb rc=0 bytes=1304 drop=0        ← 心跳记录落盘成功（诊断输出已移除）
```

- 0 次 assert / 0 次 panic；
- boot 与 hb 均正常落盘，队列无丢弃（`drop=0`）；
- 文件按预期写到 `/sdcard/LOGS/`。

### 遗留

- **TF 卡只剩 72 KB**（照片占满）。日志本身只占 1.3KB，但空间这么紧随时可能触发暂停。
  建议清理卡上的旧照片，或换更大的卡。
- 历史上有 `rst:7 (other_wdt)` 记录，对应 R-01 冷启动崩溃（bootloader 阶段
  `Instruction access fault` @ `rtc_clk_init`）。与日志模块无关，仍待 PSRAM/flash 降频 A/B。
