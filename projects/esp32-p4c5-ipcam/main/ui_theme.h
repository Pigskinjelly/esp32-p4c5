/*
 * UI 主题 —— 颜色 / 字体 / 通用小工具
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 生成的中文字体（代码中全部汉字 + ASCII） ---- */
extern const lv_font_t ui_cjk_16;
extern const lv_font_t ui_cjk_24;

/* ---- 锁屏大数字字体（96px，仅 0-9 与冒号） ---- */
extern const lv_font_t ui_font_time96;
extern const lv_font_t ui_font_time96_light;   /* 96px 细体：天气大温度用 */

/* ---- 配色 ---- */
#define C_BG        0x0A1630    /* 桌面深蓝 */
#define C_BG_CARD   0x11254D    /* 卡片 */
#define C_BG_CARD2  0x173063    /* 卡片亮一档 */
#define C_ACCENT    0x2E9BFF    /* 主色 */
#define C_CYAN      0x37D8E8    /* 大数字青色 */
#define C_UP        0xFF5A6E    /* 涨-红 */
#define C_DOWN      0x30D5A8    /* 跌-绿 */
#define C_TXT       0xFFFFFF
#define C_TXT_DIM   0x8FA8CC

static inline lv_color_t col(uint32_t c) { return lv_color_hex(c); }

/* 通用：创建一个圆角卡片容器 */
lv_obj_t *ui_card(lv_obj_t *parent, int w, int h, uint32_t bg);

/* 通用：创建标签（可指定字体/颜色，font 为 NULL 用 ui_cjk_16） */
lv_obj_t *ui_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t color);

/* 通用：App 顶栏（返回按钮 + 标题），返回时调用 ui_shell_back() */
void ui_app_topbar(lv_obj_t *parent, const char *title);

/* ---- 自绘简易键盘（不依赖 lv_keyboard：稳定、不会有方框/焦点问题） ----
 * rows: 每行一个字符串（字符即按键），如 { "1234567890", "qwertyuiop" }
 * 底部固定三键：空格 / 删除 / 确定
 */
void ui_keypad_build(lv_obj_t *parent, int y, const char *rows[], int nrows);

/** 把键盘输入绑定到某个缓冲区（bind 之后按键才生效） */
void ui_keypad_bind(char *buf, size_t cap, lv_obj_t *label,
                    void (*on_change)(void), void (*on_ok)(void));

/** 文本键盘（英文 + 中文拼音输入，带候选字条与中/英切换） */
void ui_keypad_build_text(lv_obj_t *parent, int y);

/* 缺字保护：把字体里没有的字形替换为 '?'，返回静态缓冲（短暂有效） */
const char *ui_font_safe(const char *utf8, const lv_font_t *font);

#ifdef __cplusplus
}
#endif
