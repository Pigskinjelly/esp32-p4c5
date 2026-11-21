# R-30：偶发重启的根因与处理（SDIO 接收缓冲分配失败）

日期：2026-11-20

## 现象

设备偶发重启（用户反馈"时不时重启"）。串口抓到：

```
assert failed: sdio_rx_get_buffer sdio_drv.c:953 (*buf)
Backtrace: 0x4ff2dab8 ... 0x480217b4
rst:0xc (SW_CPU_RESET)
```

`esp_reset_reason()` 返回 4（panic）。

## 根因

`managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c`
在**流式接收模式**（`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y`，默认）下，
每次收到从机中断都按"整批数据长度"调用 `_h_malloc_align(len, 64)` 申请一块
**内部 DMA 内存**，申请失败直接 `assert` → panic。

本板是 P4 + C5（Wi-Fi 走 SDIO），内部 DMA 内存被摄像头管线（SC2336 采集 / PPA 缩放 /
H.264 编码）长期占用，一旦某次 burst 需要的块比较大（几 KB~十几 KB）且内部堆碎片化，
就会 malloc 失败 —— 表现为随手点开 App、行情刷新密集时随机重启。

对照实验：同样的负载下，2 分钟内必重启 1~2 次；点开「交易」App 后更容易触发。

## 试过但不行的方案

把接收改成包模式（`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_NONE=y`，用 1536B 预分配缓冲池）：

```
assert failed: process_init_event transport_drv.c:881 (0)
```

开机就死循环重启 —— 本机 C5 从机固件只支持流式模式，握手不兼容。**不要改这个配置**。

## 实际采用

在 `sdio_drv.c` 打两处本地补丁（保留流式模式）：

1. `sdio_rx_get_buffer()` 分配失败时**返回 NULL**，并打印剩余内部堆，不再 assert；
2. 调用处 `assert(rxbuff)` 改为：解锁 → 延时 10ms → `continue`。

数据仍留在从机队列里，下一轮继续读，所以只是**偶发一次毫秒级延迟**，不会丢包、
不会重启。

> ⚠️ 这两个补丁改在 `managed_components/` 里。如果以后执行
> `idf.py update-dependencies` / 删除 managed_components，补丁会丢，需要重新打。

## 配套的诊断改进

`main.c` 启动时会打印并累计：

```
W (1942) ipcam: reset reason: 1(掉电/复位键) | 累计启动 5 次 / 异常 2 次
E (1867) ipcam: *** 上次是异常重启（看门狗/panic）***
```

- 计数存在 NVS `sys/boots`、`sys/crashes`
- 心跳日志加了内部堆最低水位：`heap_int=119 KB (min 33 KB)`

下次再重启，先看这两行就能判断是掉电、还是 panic/WDT。

## 最终方案（实测通过）

1. **开机预分配**：`sdio_rx_prealloc()` 在 `double_buf` 清零之后，一次性分配
   2 × 8KB 的接收缓冲（12KB 太吃内部堆，会让 heap 最低水位掉到 9KB）。
2. **分配失败兜底**：`sdio_rx_get_buffer()` 先重试 3 次（每次 5ms），仍失败就
   用 512B 对齐的栈缓冲把这一批数据**读掉丢弃**，保持 SDIO 流不错位。
   （第一版只是 `continue` 不读，结果长度寄存器已被取走、整条流错位，
   Wi-Fi 全挂、sdmmc 报 0x107、最后 `Unrecoverable host sdio state` 重启。）

实测：改造前 110 秒内崩 1~3 次；改造后连续 130 秒跑「交易」App 自测
（指数四个周期 + 切美股 + 美股指数/个股 K 线）**0 次崩溃、0 次断言**。

## 附带发现：腾讯 K 线接口在设备上不可用

设备请求 `web.ifzq.gtimg.cn` 只回 137 字节（PC 上同样 URL 正常 15KB），
导致周/月 K 一直失败。K 线主源已换成**东方财富**：

```
push2his.eastmoney.com/api/qt/stock/kline/get?secid=1.000001&klt=101&fqt=1&lmt=180
```

- A 股：`1.000001`（沪）/ `0.000001`（深）
- 美股个股：`105.AAPL`（纳斯达克）/ `106.BABA`（纽交所），按 105→106→107 试
- 美股指数：`100.DJIA` 道琼斯 / `100.NDX` 纳指100 / `100.SPX` 标普500
- `klt`：101 日 / 102 周 / 103 月；年 K 用月线本地合并

腾讯/新浪保留为 A 股兜底。
