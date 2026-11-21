#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 起后台任务：联网后抓当年+次年的法定节假日，缓存到 NVS（重复调用无副作用） */
void trade_day_init(void);
/** 触发一次刷新（同样只保证任务在跑） */
void trade_day_refresh(void);

/** 今天 yyyymmdd（时间没同步时返回 0） */
int  trade_day_today(void);
/** 是否交易日（周末 / 法定假日休市，调休上班日照常交易） */
bool trade_day_is_trading(int ymd);
/** 从 ymd 往后数 n 个交易日，返回那天的 yyyymmdd */
int  trade_day_add(int ymd, int n);
/** (from, to] 之间的交易日数量 */
int  trade_day_count(int from_ymd, int to_ymd);
/** 是否已经拿到网络日历（false 时只按周末判断，UI 上要提示） */
bool trade_day_ready(void);

#ifdef __cplusplus
}
#endif
