/*
 * 设备运行日志 —— 落盘到 TF 卡（无卡时回落 flash 的 storage 分区）
 *
 * 为什么要有它
 *   以前排查问题只能靠串口，拔了线就什么都没了；偶发重启、卡顿、内存见底
 *   这类问题往往发生在我们不在电脑前的时候。这个模块把关键状态持续写到
 *   TF 卡，事后直接读文件就能还原现场。
 *
 * 格式：JSONL（一行一条 JSON），UTF-8，便于 grep / jq / Python 解析
 *   目录：<存储根>/LOGS/run-YYYYMMDD.jsonl     每天一个文件
 *   容量：单文件超过 4 MB 自动切到 run-YYYYMMDD-2.jsonl，最多保留 14 天
 *
 * 一条记录的字段（顺序固定，缺失的键不出现）
 *   t     本地时间 "2026-11-22 17:30:12.345"（时间未同步时写 "-"）
 *   up    上电秒数，单调递增，跨文件也能对齐时间轴
 *   lvl   I / W / E
 *   mod   boot | sys | wifi | cam | net | stk | wx | ui
 *   ev    事件名，见下表
 *   其余为 k=v 形式的附加字段（数字不加引号，字符串加引号）
 *
 * 已实现的事件
 *   boot    启动：固件版本、复位原因、累计启动/异常次数
 *   crash   上次是异常复位（panic / 任务看门狗 / 中断看门狗 / 欠压）
 *   hb      心跳：内部堆(当前/最低)、PSRAM、WiFi、RTSP 客户端、编码帧数
 *   stall   卡顿：日志任务自身被延迟 > 250 ms（说明系统整体被抢占/阻塞）
 *   heap_low 内部堆低于告警阈值
 *   wifi    WiFi 状态变化（含 IP）
 *   sd      日志文件切换 / 存储不可用
 *
 * 用法
 *   runlog_init();                                  // 存储挂载后调一次
 *   runlog_boot();                                  // 启动时调一次（内部会判断 crash）
 *   runlog_event(RUNLOG_W, "sys", "x", "a=%d", 1);  // 任意事件
 *   runlog_heartbeat(...);                          // 心跳任务里调
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RUNLOG_I = 0,
    RUNLOG_W,
    RUNLOG_E,
} runlog_level_t;

/** 初始化：建目录、打开当天文件、清理过期文件、启动写任务。幂等。 */
bool runlog_init(void);

/** 是否已经能写（存储可用且任务在跑） */
bool runlog_ready(void);

/** 当前日志文件路径（没准备好时返回空串），给设置页/日志里显示 */
const char *runlog_path(void);

/** 记一条事件。fmt 为附加字段的 printf 片段（可以为 NULL） */
void runlog_event(runlog_level_t lvl, const char *mod, const char *ev,
                  const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/**
 * 心跳（每 30 秒调一次）。内部会对比上次状态，自动记录 WiFi 变化和低内存。
 * frames/keyframes/last_size 来自编码器，pkts/bytes/clients 来自 RTSP。
 */
void runlog_heartbeat(uint32_t frames, uint32_t keyframes,
                      int rtsp_clients, uint32_t rtsp_pkts,
                      const char *wifi_state, const char *ip);

/**
 * 记启动事件：固件版本、复位原因、累计启动/异常次数；异常复位时额外记 crash 事件。
 * 参数由 main.c 传入（那边的 NVS 计数逻辑已经算好了，避免重复读写）。
 * 需要在 runlog_init() 之后、其他事件之前调用。
 */
void runlog_boot(int rst_reason, uint32_t boots, uint32_t crashes);

/** 把当前日志文件最后 lines 行打到串口（现场快速看，不依赖读卡） */
int runlog_dump(int lines);

#ifdef __cplusplus
}
#endif
