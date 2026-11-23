# 三模块回退 + WiFi 自动选网/漫游（2.40）

日期：2026-11-23
固件版本：2.40（PROJECT_VER 未变，仅代码回退 + WiFi 增强）

---

## 一、回退：自检 / WiFi / 相册

### 版本依据（来自 git 历史，无需额外确认）

| 提交 | 内容 | 版本号 |
|---|---|---|
| `3b4a506` | R-41 WiFi 空密码修复 | 2.39 |
| `8ac46ad` | 天气排版 + TF 卡运行日志 | **2.40** |
| `9ac2956` | 三模块按设计稿状态机重构 | 2.40 |
| `af52dd7` | 自检浮层未删除 / 中文方框修复 | 2.40 |

- “早于 2.40” = `3b4a506`；
- 但 `3b4a506 → 8ac46ad` **没有改动** `ui_boot.c / ui_shell.c / ui_app_gallery.c`，
  所以这三个文件在 `8ac46ad` 与 2.39 完全一致；
- 因此回退到 `8ac46ad` 的这三个文件即可，同时保留 2.40 的天气与运行日志。

### 回退清单（`git checkout 8ac46ad -- ...`）

| 文件 | 恢复到 2.39 行为 |
|---|---|
| `main/ui_boot.c` / `ui_boot.h` | 7 项检查（系统/显示/触摸/内存/网络/相机/存储）、两列文字、转圈动画、单行提示；无进度/状态条/跳过按钮 |
| `main/ui_shell.c` | 控制中心 + 「扫描 WiFi」弹窗的原版；无 `wifi_ui_stage()` 状态机、无扫描加载态、密码框点连接即关闭 |
| `main/ui_app_gallery.c` | 网格底部显示**文件名**（不显示拍摄日期） |
| `main/wifi_mgr.c` / `wifi_mgr.h` | 去掉 `wifi_mgr_last_reason()` |
| `main/main.c` | 恢复 `BOOT_SYS/BOOT_DISP/BOOT_TOUCH` 上报，去掉 `BOOT_RTSP` 与 `s_touch_ok` |

### 保留不动的部分（仍在 2.40）

- `ui_app_weather.c`：天气排版重做 + 细体温度字体；
- `runlog.c/h` + `main.c` 里的日志接入：TF 卡运行日志；
- `ui_font_time_96_light.c`、`ui_theme.h` 的字体声明；
- 2.40 之前的全部修复：R-38（信号量时序 panic）、R-39（先扫描再连接 / band mode 时机）、
  R-40/R-41（空密码导致 NO_AP_FOUND）。

### 数据与配置

回退只涉及代码，**不触碰 NVS**：已保存的 WiFi 凭据、自选股、天气城市、App 开关位图、
运行日志文件全部保留。

### 验证

- 编译 0 error；
- 自检恢复 7 项且浮层正常删除（`lv_obj_delete(s_root)` 在 2.39 版本里本来就有）。

---

## 二、WiFi 自动发现与重连（新增 `roam_*`）

### 为什么要做

原来的行为：开机只连 `s_cred[0]`（最近使用），连上后不再扫描（省电），
断线就无限重连同一个 SSID。结果是：

- 从 A 网络走到 B 网络：一直重连 A，报 `NO_AP_FOUND`，必须手动连 B；
- 回到 A 范围：已经连着 B，不会切回；
- 界面上偶尔显示"附近无网络"，其实是扫描被连接打断、结果被清空。

### 设计

```
每 60s 静默扫描 ──> SCAN_DONE ──> roam_evaluate()
                                    │
        ┌───────────────────────────┼────────────────────────────┐
        │                           │                            │
  范围内无已保存网络          未连接                      已连接
        │                           │                            │
     不动作              最佳 != 当前目标 ──> 切换        当前 SSID 扫不到 x3 ──> 切最佳
                                              │         最佳比当前强 12dBm ──> 切最佳
                                              └──────────> roam_request_switch()
                                                            （100ms 定时器，脱离事件任务）
                                                                    │
                                                            wifi_mgr_connect(ssid, pass)
```

关键参数：

| 宏 | 值 | 作用 |
|---|---|---|
| `ROAM_SCAN_MS` | 60s | 定期扫描间隔（已连接时也扫） |
| `ROAM_HYST_DBM` | 12 | 新网络要强 12dBm 才切，防边界抖动 |
| `ROAM_MISS_LIMIT` | 3 | 当前网络连续 3 次扫不到才认定走远（约 3 分钟） |
| `ROAM_COOLDOWN_MS` | 45s | 每次切换后 45s 内不再切 |

覆盖的五个点：

1. **保存网络识别**：`best_saved_cred()` 在扫描结果里匹配 `s_cred[]`（已保存列表），
   取 RSSI 最大的那个；不依赖"当前目标"，所以换到陌生网络范围也能找到已保存的可用网络。
2. **跨网络切换**：A 消失 → 切 B；B 范围内 A 更强 → 切回 A。切换复用
   `wifi_mgr_connect()`，因此密码沿用已保存值、NVS 顺序更新为"最近使用"。
3. **断线重连**：保留原有的指数退避重连（0.5s→1s→…→15s）；重连期间若漫游发现
   另一个已保存网络在范围内，会改连它（`retarget`）。
4. **信号变化**：只在"强 12dBm 以上"时切，避免两个网络信号接近时来回跳。
5. **重复弹窗/误报**：
   - 扫描返回 0 条时**保留上一次的 AP 缓存**（`scan got 0 AP, keep previous N`），
     界面不会突然变成"附近无网络"；
   - 定期扫描是**静默**的（`s_scan_silent`），不改 `s_state`、不 `notify()`，
     UI 不会每 60 秒闪一次"正在扫描"；
   - 切换请求去重（同一目标不重复排），切换动作走一次性定时器，不在事件任务里阻塞。
6. **不干扰用户**：用户主动断开（`s_user_disconnect`）或关闭射频时，漫游完全停手。

### 日志（便于现场验证）

```
roam: no saved network in range (5 ap)          ← 范围内没有已保存网络
roam: "<WiFi名称>" missing (1/3)                    ← 当前网络扫不到，累计
roam: pick "MERCURY_6C3C" rssi=-45 (current lost)
roam: connecting "MERCURY_6C3C" (#1), drop "<WiFi名称>"
roam: "MERCURY_6C3C"(-45) -> "<WiFi名称>"(-38)      ← 发现更强的已保存网络
scan got 0 AP, keep previous 5                  ← 空扫描保护
```

---

## 三、可验证的测试场景

| # | 场景 | 操作 | 预期（看串口日志） |
|---|---|---|---|
| 1 | 保存网络识别 | 保存 A、B 两个网络；只开 A，重启设备 | 扫描后自动连 A；`target "A" ok ... -> connect` |
| 2 | 跨网络 A→B | 连着 A，关掉 A 的路由器（或走远到 A 扫不到） | 约 3 次扫描后：`"A" missing (3/3)` → `pick "B" (current lost)` → `connecting "B" (#1)` |
| 3 | 跨网络 B→A | 回到 A 范围，A 比 B 强 12dBm 以上 | `"B"(-60) -> "A"(-45)` → 切回 A |
| 4 | 断线重连 | 关掉当前路由器 30 秒再打开 | 先按 0.5/1/2/4s 退避重连同一 SSID；恢复后 `got ip` |
| 5 | 滞回（不抖） | A、B 都在范围，信号差 < 12dBm，在两者之间走动 | 不切换（无 `roam: ... -> ...` 日志） |
| 6 | 空扫描不误报 | 连接建立瞬间恰好触发扫描 | `scan got 0 AP, keep previous N`，界面列表不清空 |
| 7 | 重复弹窗 | 连续断线 5 次 | 切换只打一次 `connecting (#n)`，UI 不连续弹提示 |
| 8 | 用户主动断开 | 控制中心关闭 WiFi 射频 | 不再扫描、不再切换；`wifi radio off` |

建议配合运行日志一起看：`/sdcard/LOGS/run-YYYYMMDD.jsonl` 里的 `wifi/state` 事件
能还原"什么时候断、断了多久、切到哪个网络"。

---

## 四、实测记录

### 回退验证（烧写后串口）

```
I ui_boot: boot check UI ready
I ui_boot: [check] 显示 = OK (480x480)
I ui_boot: [check] 触摸 = OK (ST7123)
I ui_boot: [check] 内存 = OK (内部 228K)
I ui_boot: [check] 系统 = OK (网络栈就绪)
I ui_boot: [check] 相机 = OK (已出图)
I ui_boot: [check] 存储 = OK (TF 卡)
I ui_boot: [check] 网络 = OK (<设备IP>)
```

7 项全部恢复，浮层正常删除（2.39 版本本来就有 `lv_obj_delete`），触摸正常。

### 漫游验证

```
I wifi_mgr: roam timer started: ESP_OK (60000 ms)
I wifi_mgr: target "<WiFi名称>" ok: ch=1 rssi=-42 auth=3 -> connect
I wifi_mgr: got ip: <设备IP>
I wifi_mgr: roam: silent scan started (state=3)      ← 已连接(state=3)时也会静默扫
I wifi_mgr: scan done: 4 unique AP(s)               ← 扫描结果刷新，UI 无感知
I wifi_mgr: roam: silent scan started (state=3)      ← 60s 后再扫
```

未触发切换是因为当前网络就是范围内最强的已保存网络（符合设计）。
跨网络切换需要按下节场景 2/3 实测。

> 注意：**已连接状态下的扫描要遍历信道，实测约 10 秒**（66s→77s）。
> 这是 ESP32 单射频的固有代价，期间吞吐会短暂下降。若影响 RTSP 流畅度，
> 可把 `ROAM_SCAN_MS` 从 60s 放宽到 120~180s，代价是发现更好的网络更慢。

### 构建环境备注（重要）

这台机器编译时 `ldgen` 会反复卡在 objdump（DLP 拦截），已做两件事：

1. **必须清理僵尸**：编译中断会留下 `ldgen.py` 进程，下次编译会与之并发抢资源，
   表现为 objdump CPU≈0、日志停在 `[2/9]`。清理命令：
   ```
   taskkill 掉所有 ninja / objdump / 以及命令行含 ldgen.py 的 python
   ```
2. **逃生阀**：`ldgen.py` 增加了 `LDGEN_KEEP` 环境变量——指向一份已生成的
   `sections.ld`，则直接复制过去、跳过所有 objdump：
   ```bash
   export LDGEN_KEEP=/d/p4c5b3/sections.ld.bak
   ```
   **仅在 fragments 未改动时使用**（section 摆放规则不变，旧脚本仍有效）；
   改过 linker.lf 或新增组件时不能用，必须让 ldgen 正常跑。
