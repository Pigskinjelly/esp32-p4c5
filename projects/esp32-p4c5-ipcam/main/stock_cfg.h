/*
 * 交易 App 的运行参数（设置 → 应用管理 → 交易 里调）
 *
 *   front : App 内行情刷新间隔（秒），1 / 3 / 5
 *   bg    : 退出 App 后的后台刷新间隔（秒），3 / 5 / 10
 *           后台刷新就是"后台弹窗提醒"的频率：价格提醒靠拉行情触发，
 *           拉得越勤，到价提醒得越快。
 *
 * 存 NVS 命名空间 "cfg"：stk_front / stk_bg
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** 读一次 NVS（可重复调用，只读一次） */
void stock_cfg_load(void);

/** App 内行情刷新间隔（秒），默认 1 */
int  stock_cfg_front(void);
/** 后台刷新间隔（秒），默认 3 */
int  stock_cfg_bg(void);

void stock_cfg_set_front(int sec);
void stock_cfg_set_bg(int sec);

#ifdef __cplusplus
}
#endif
